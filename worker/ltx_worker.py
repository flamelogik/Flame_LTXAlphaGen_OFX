"""LTX-2.5 Alpha Gen worker (the real one). Runs in the LTX venv.

Three modes:
  one-shot (job.py legacy / smoke tests):
      --in F --chunks s:l,... --out-prefix P --model-dir M --embeddings E
      [--precision bf16|fp8] [--colour Auto|<colour.SOURCES key>] [--colour-tag T] [--ocio-config C]
      [--hold K | --holds K,K,..]
      stdout lines as in dummy_worker.py: info / progress / chunk_done / done / error
  serve (resident worker, decisions 11/19/33):
      --serve SOCKET --model-dir M --embeddings E --precision P [--flame-pid N]
      [--idle-timeout S]
      Loads the model once, then serves jobs over a Unix socket. Each connection sends one
      JSON request line {"in", "chunks", "out_prefix", "colour", "colour_tag", "ocio_config", "precision",
      "model_dir", "unload", "seed", "holds"} and receives the same stdout-style lines back. A request
      whose precision/model_dir differ gets "restart" and the server exits, so the client
      starts a matching one. The server exits after a job with "unload": true, after
      --idle-timeout seconds without jobs (default 300; $LTXAG_IDLE_TIMEOUT overrides), or
      when --flame-pid dies. SOCKET.json holds {pid, precision, model_dir, state, vram_gb}.
  --build-embeddings (child process, see build_embeddings).
  --fp16-probe / --fp16-convert SRC DST (run by Setup on macOS, decision 68; ltxag_worker/fp16.py).
Precision "fp16" (macOS, decision 68): the bf16 pipeline with the transformer, its denoising state,
the prompt embedding and the reference latent in fp16, from the fp16 copy Setup made; the VAE
decode stays bf16. Same matte as bf16 (mean difference 0.0002), 1.40x faster steps on an M1.

Recipe 1 (decisions 15/16): ICLoraPipeline, skip_stage_2 with 2x width/height, empty prompt,
LoRA 1.0. Injections into the pinned LTX-2 code (commit 9ec55f9):
  - ic_lora.SequentialVideoFrameSource -> MemoryFrameSource (in-memory reference frames);
  - pipeline.prompt_encoder -> CachedPromptEncoder (no Gemma per job, decision 9);
  - DiffusionStage._transformer_ctx -> build once, keep for the engine's lifetime
    (upstream "builds on each call, frees on exit": 6.6 s per chunk at HD fp8). Safe because
    the single-GPU Builder.build ignores the per-call video_tools kwarg (ARG002);
  - pipeline.audio_decoder = None: video-only chunks (chunks/operators.py:285), saving ~2 s/chunk;
  - samplers.tqdm -> a stepping wrapper that reports each denoising step (there is no callback;
    euler_denoising_loop iterates tqdm(sigmas[:-1]), utils/samplers.py:72);
  - MPS only: pipeline.image_conditioner -> ReferenceEncoderInDtype(bf16 or fp32, whichever a
    conv3d probe at start finds faster, pick_encode_dtype). M1 has no fast bf16: a bf16 conv3d
    runs at 0.40 TFLOPS vs 0.92 fp32 (4.7 fp16, which overflows to NaN in this encoder), so the
    bf16 reference encode took 285 s of a 887 s 720p x 49 job; M2 and later have hardware bf16.
    The latent goes back to bf16 for the rest of the pipeline. It also waits for the GPU after every conv3d:
    work already submitted runs to completion after a SIGKILL (measured: 100 % for ~20 s after
    the process was gone), and the encode is one call, so Cancel left the whole encode running.
Progress lines (besides info/chunk_done/done/error): "stage <chunk> <name>" (converting,
preparing, denoising, decoding), "step <chunk> <k> <n>" after each denoising step, and at the
end "vram <min_free_gb> <peak_alloc_gb> <peak_reserved_gb> <reserved_at_start_gb> <torch>": the lowest
free device memory sampled during the job (every 0.25 s), for comparing the plugin's estimate
with what the job really took.
Device (decision 65): CUDA on Linux, MPS (Apple Silicon) on macOS, through Accel. On MPS "free"
is Metal's recommended working set minus what this process holds (as upstream's
ltx_core/devices.py:mps_activation_budget_bytes), and the peaks are sampled: torch.mps has no
peak statistics.
  A ModelRegistry(cache_weights=True) was tried first and was worse (chunk 1 200 s, +16 GB).
Frames arrive in the source colour space as float16; colour.Converter maps them to Rec.1886
display values (decision 54), then they are scaled to [0,255] floats like the pipeline's reader.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import socket
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# Same allocator setting as the measured CLI runs; must be set before torch initialises CUDA.
os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")

import numpy as np  # noqa: E402

from ltxag_worker import colour, fp16, padding, transport  # noqa: E402
from ltxag_worker.log import Logger  # noqa: E402

MEMORY_REF = "mem://ltxag-reference"
SEED_DEFAULT = 1234
FRAME_RATE = 24.0  # only feeds the model's timing embedding; mattes do not depend on it here
IDLE_TIMEOUT_DEFAULT = 300.0  # decision 33

log = Logger("worker")


class Accel:
    """The GPU this worker runs on: CUDA (Linux) or MPS (macOS, decision 65). Memory in GB."""

    def __init__(self, torch) -> None:  # noqa: ANN001 - the torch module
        self.torch = torch
        if torch.cuda.is_available():
            self.kind = "cuda"
        elif torch.backends.mps.is_available():
            self.kind = "mps"
        else:
            raise RuntimeError("no CUDA or MPS GPU is available to the worker")
        self.device = torch.device(self.kind)
        self.peak = [0.0, 0.0]  # MPS only: sampled allocated, reserved

    def describe(self) -> str:
        t = self.torch
        if self.kind == "cuda":
            return f"torch {t.__version__} cuda {t.version.cuda} gpu {t.cuda.get_device_name(0)}"
        return f"torch {t.__version__} mps, recommended working set {t.mps.recommended_max_memory() / 2**30:.1f} GB"

    def empty_cache(self) -> None:
        (self.torch.cuda if self.kind == "cuda" else self.torch.mps).empty_cache()

    def reserved_gb(self) -> float:
        t = self.torch
        return (t.cuda.memory_reserved() if self.kind == "cuda" else t.mps.driver_allocated_memory()) / 2**30

    def allocated_gb(self) -> float:
        t = self.torch
        return (t.cuda.memory_allocated() if self.kind == "cuda" else t.mps.current_allocated_memory()) / 2**30

    def free_gb(self) -> float:
        t = self.torch
        if self.kind == "cuda":
            return t.cuda.mem_get_info()[0] / 2**30
        return (t.mps.recommended_max_memory() - t.mps.driver_allocated_memory()) / 2**30

    def sample(self) -> float:
        """Free memory now; on MPS also updates the sampled peaks."""
        if self.kind == "mps":
            self.peak = [max(self.peak[0], self.allocated_gb()), max(self.peak[1], self.reserved_gb())]
        return self.free_gb()

    def reset_peaks(self) -> None:
        if self.kind == "cuda":
            self.torch.cuda.reset_peak_memory_stats()
        else:
            self.peak = [self.allocated_gb(), self.reserved_gb()]

    def peaks_gb(self) -> tuple[float, float]:
        t = self.torch
        if self.kind == "cuda":
            return t.cuda.max_memory_allocated() / 2**30, t.cuda.max_memory_reserved() / 2**30
        self.sample()
        return self.peak[0], self.peak[1]


class Cancelled(Exception):
    """The client went away mid-job."""


class MemoryFrameSource:
    """Drop-in for ltx_pipelines.chunks.export.SequentialVideoFrameSource (take/close).

    The chunk's frames stay in host memory and go to the GPU one at a time, as upstream's
    decoder delivers them (chunks/export.py). Holding the whole chunk on the GPU as float32 cost
    ~3.6 GB at HD x 145 (Flame test 2: an HD job used 8.6 GB more than estimated)."""

    frames = None  # torch tensor (N, H, W, 3) float in [0,255], host memory (or GPU with --frames-on-gpu), set per chunk

    def __init__(self, path: str, device) -> None:  # noqa: ANN001 - torch.device
        if path != MEMORY_REF:
            raise ValueError(f"MemoryFrameSource only serves {MEMORY_REF}, got {path}")
        self.device = device

    def take(self, start: int, count: int):
        f = MemoryFrameSource.frames
        return (f[i : i + 1].to(self.device) for i in range(start, min(start + count, f.shape[0])))

    def close(self) -> None:
        pass


class ReferenceEncoderInDtype:
    """Stands in for ImageConditioner (blocks.py:948): builds the reference video encoder in
    another dtype and casts frames in / latents out, so the rest of the pipeline is unchanged.
    With `sync`, it is called after every Conv3d, so little GPU work is queued at any time."""

    class _Cast:
        def __init__(self, enc, dtype, sync) -> None:  # noqa: ANN001
            self.enc, self.dtype, self.sync = enc, dtype, sync

        def __call__(self, video):  # noqa: ANN001
            import torch

            hooks = [m.register_forward_hook(lambda *_: self.sync())
                     for m in self.enc.modules() if self.sync and isinstance(m, torch.nn.Conv3d)]
            try:
                return self.enc(video.to(self.dtype)).to(video.dtype)
            finally:
                for h in hooks:
                    h.remove()

        def __getattr__(self, name: str):
            return getattr(self.enc, name)

    def __init__(self, inner, dtype, sync=None) -> None:  # noqa: ANN001 - ImageConditioner, torch.dtype
        inner._dtype = dtype  # the dtype its Builder builds the encoder in (blocks.py:998)
        self.inner, self.dtype, self.sync = inner, dtype, sync

    def resolve_crf(self, images):  # noqa: ANN001
        return self.inner.resolve_crf(images)

    def __call__(self, fn):  # noqa: ANN001
        return self.inner(lambda enc: fn(ReferenceEncoderInDtype._Cast(enc, self.dtype, self.sync)))


def pick_encode_dtype(torch, device):  # noqa: ANN001, ANN201 - the torch module, torch.device
    """MPS: times one encoder-sized Conv3d in bf16 and fp32 and returns (faster dtype, bf16 ms,
    fp32 ms). The M1 has no fast bf16 (fp32 wins), M2 and later have hardware bf16 (decision 65).
    A tie goes to bf16: upstream's dtype, half the encoder memory."""
    conv = torch.nn.Conv3d(128, 128, 3, padding=1)
    times = {}
    for dtype in (torch.bfloat16, torch.float32):
        c = conv.to(device, dtype)
        x = torch.randn(1, 128, 5, 64, 64, device=device, dtype=dtype)
        with torch.inference_mode():
            c(x)  # warm-up: kernel selection
            torch.mps.synchronize()
            t0 = time.perf_counter()
            for _ in range(3):
                c(x)
            torch.mps.synchronize()
        times[dtype] = (time.perf_counter() - t0) / 3 * 1000
    del c, x
    best = torch.float32 if times[torch.float32] < times[torch.bfloat16] else torch.bfloat16
    return best, times[torch.bfloat16], times[torch.float32]


class CachedPromptEncoder:
    """Stands in for PromptEncoder: returns the stored empty-prompt embedding."""

    def __init__(self, output) -> None:  # noqa: ANN001 - EmbeddingsProcessorOutput
        self.output = output

    def __call__(self, prompts, **_kwargs):  # noqa: ANN001
        if list(prompts) != [""]:
            raise ValueError("cached embedding only covers the empty prompt")
        return [self.output]


def model_paths_for(m: str) -> dict:
    return {
        "transformer": f"{m}/diffusion_models/ltx-2.5-22b-distilled-transformer-bf16.safetensors",
        "text_encoder": f"{m}/text_encoders/gemma4-12b-with-proj-ltx-2.5-bf16.safetensors",
        "video_vae": f"{m}/vae/ltx-2.5-video-vae-bf16.safetensors",
        "audio_vae": f"{m}/vae/ltx-2.5-audio-vae-bf16.safetensors",
        "upsampler": f"{m}/latent_upscale_models/ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors",
        "lora": f"{m}/loras/ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors",
    }


def fp16_transformer_path(model_dir: str) -> str:
    return model_paths_for(model_dir)["transformer"].replace("-bf16.safetensors", "-fp16.safetensors")


def file_id(path: str) -> str:
    st = os.stat(path)
    return f"{os.path.basename(path)}:{st.st_size}"


def embeddings_ident(transformer: str, text_encoder: str) -> str:
    return f"{file_id(transformer)}|{file_id(text_encoder)}"


def build_embeddings(path: str, transformer: str, text_encoder: str, video_vae: str) -> int:
    """Child-process mode: run Gemma once for the empty prompt, save, exit.

    Its own process because Gemma's ~26 GB were not fully returned before the transformer
    loaded when this ran in-process (smoke test: OOM at 94 GB vs 45.5 GB without).
    """
    import torch
    from safetensors.torch import save_file

    from ltx_pipelines.utils.blocks import PromptEncoder
    from ltx_pipelines.utils.model_paths import ModelPaths

    t0 = time.time()
    model_paths = ModelPaths.from_split(transformer_path=transformer, text_encoder_path=text_encoder,
                                        video_vae_path=video_vae)
    encoder = PromptEncoder(model_paths, torch.bfloat16, Accel(torch).device)
    with torch.inference_mode():
        (out,) = encoder([""])
    tensors = {"video_encoding": out.video_encoding, "attention_mask": out.attention_mask}
    if out.audio_encoding is not None:
        tensors["audio_encoding"] = out.audio_encoding
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.tmp{os.getpid()}"
    save_file({k: v.contiguous().cpu() for k, v in tensors.items()}, tmp,
              metadata={"source": embeddings_ident(transformer, text_encoder), "prompt": ""})
    os.replace(tmp, path)
    msg = (f"prompt embedding built in {time.time() - t0:.1f} s: {path} "
           f"(video_encoding {tuple(out.video_encoding.shape)} {out.video_encoding.dtype})")
    log.info(msg)
    print("info", msg, flush=True)
    return 0


class Engine:
    """The loaded pipeline. One per process; serves any number of jobs."""

    def __init__(self, model_dir: str, embeddings: str, precision: str, emit, keep_transformer: bool = True,
                 frames_on_gpu: bool = False):
        import torch

        import ltx_pipelines.ic_lora as ic_lora
        from ltx_core.loader import LTXV_LORA_COMFY_RENAMING_MAP, LoraPathStrengthAndSDOps
        from ltx_pipelines.utils.model_paths import ModelPaths
        from ltx_pipelines.utils.quantization_factory import QuantizationKind

        self.torch = torch
        self.accel = Accel(torch)
        self.model_dir, self.precision = model_dir, precision
        self.frames_on_gpu = frames_on_gpu  # A/B only: the pre-2026-10-11 behaviour
        self.fp16 = precision == "fp16"  # decision 68
        t0 = time.time()
        emit("info", self.accel.describe())
        paths = model_paths_for(model_dir)
        missing = [k for k, p in paths.items() if not os.path.exists(p)]
        if missing:
            raise FileNotFoundError(f"model files missing in {model_dir}: {', '.join(missing)}")
        # Before the pipeline exists, so a cache build (child process, Gemma) has the GPU to itself.
        embedding = self._load_or_build_embeddings(embeddings, paths, emit)
        transformer_file = paths["transformer"]
        if self.fp16:
            # The copy Setup made (ltxag_worker/fp16.py) loads like the bf16 file; casting at build
            # instead held ~26 GB more. No silent fallback to bf16 (decision 34).
            transformer_file = fp16_transformer_path(model_dir)
            if not os.path.exists(transformer_file):
                raise FileNotFoundError(f"the fp16 copy of the transformer is missing ({transformer_file}); "
                                        "press Download on the Setup tab to make it again")
            emit("info", f"fp16 transformer from {transformer_file}")
        ic_lora.SequentialVideoFrameSource = MemoryFrameSource  # injection 1
        quant = QuantizationKind("fp8-cast").to_policy(paths["transformer"]) if precision == "fp8" else None
        self.pipeline = ic_lora.ICLoraPipeline(
            model_paths=ModelPaths.from_split(
                transformer_path=transformer_file, text_encoder_path=paths["text_encoder"],
                video_vae_path=paths["video_vae"], audio_vae_path=paths["audio_vae"]),
            spatial_upsampler_path=paths["upsampler"],
            loras=[LoraPathStrengthAndSDOps(paths["lora"], 1.0, LTXV_LORA_COMFY_RENAMING_MAP)],
            quantization=quant)
        if self.fp16:
            self.pipeline.dtype = torch.float16  # latents, noise and reference frames (ic_lora.py:312)
            self.pipeline._diffusion_stage._dtype = torch.float16  # the denoising state (blocks.py:565)
            embedding = type(embedding)(embedding.video_encoding.half(), embedding.audio_encoding.half()
                                        if embedding.audio_encoding is not None else None, embedding.attention_mask)
        self.pipeline.prompt_encoder = CachedPromptEncoder(embedding)  # injection 2
        self.pipeline.audio_decoder = None  # injection 4: video-only output
        if self.accel.kind == "mps":  # injection 6: reference encode in the chip's faster dtype
            enc_dtype, ms_bf16, ms_fp32 = pick_encode_dtype(torch, self.accel.device)
            emit("info", f"reference encode in {str(enc_dtype).removeprefix('torch.')} "
                         f"(conv3d probe: bf16 {ms_bf16:.1f} ms, fp32 {ms_fp32:.1f} ms)")
            self.pipeline.image_conditioner = ReferenceEncoderInDtype(self.pipeline.image_conditioner, enc_dtype,
                                                                      sync=torch.mps.synchronize)
        self._on_step = lambda k, n: None
        self._install_step_hook()  # injection 5
        self._transformer, self._transformer_key = None, None
        if keep_transformer:  # injection 3
            # Class-level, keyed by checkpoint + LoRA set: ICLoraPipeline builds its base stage
            # with loras=() and applies the IC-LoRA through stage.with_loras(), which copy.copy()s
            # the stage. An instance-level patch was inherited by that copy and built the base
            # model WITHOUT the LoRA (smoke test: the "matte" reproduced the input picture).
            from ltx_pipelines.utils import blocks

            original_ctx = blocks.DiffusionStage._transformer_ctx
            engine = self

            def resident_ctx(stage, **kwargs):
                if stage._is_streaming:
                    return original_ctx(stage, **kwargs)
                builder = stage._transformer_builder
                key = (str(builder.checkpoint), tuple((str(l.path), float(l.strength)) for l in builder.loras))
                if engine._transformer_key != key:
                    engine.drop_transformer()
                    t1 = time.time()
                    engine._transformer = stage._build_transformer(**kwargs)
                    engine._transformer_key = key
                    emit("info", f"transformer built in {time.time() - t1:.1f} s with "
                                 f"{len(key[1])} LoRA(s); kept resident")
                return contextlib.nullcontext(engine._transformer)

            blocks.DiffusionStage._transformer_ctx = resident_ctx
        self.accel.empty_cache()
        emit("info", f"engine ready in {time.time() - t0:.1f} s (precision {precision}, "
                     f"transformer {'resident' if keep_transformer else 'rebuilt per chunk'})")

    def _install_step_hook(self) -> None:
        from ltx_pipelines.utils import samplers

        engine = self

        def stepping(iterable, *_args, **_kwargs):
            n = len(iterable)
            for k, item in enumerate(iterable):
                yield item
                engine._on_step(k + 1, n)  # runs when the loop asks for the next step: step k done

        samplers.tqdm = stepping

    def drop_transformer(self) -> None:
        """Free the resident transformer (after an OOM, so the next job starts clean)."""
        self._transformer, self._transformer_key = None, None
        self.accel.empty_cache()

    def _load_or_build_embeddings(self, path: str, paths: dict, emit):
        from safetensors import safe_open
        from safetensors.torch import load_file

        from ltx_core.text_encoders.gemma.embeddings_processor import EmbeddingsProcessorOutput

        ident = embeddings_ident(paths["transformer"], paths["text_encoder"])
        current = False
        if os.path.exists(path):
            with safe_open(path, framework="pt") as f:
                current = (f.metadata() or {}).get("source") == ident
            if not current:
                emit("info", "prompt embedding cache was built for other model files; rebuilding")
        if not current:
            emit("progress", 0.0, "building prompt cache (one-time, loads Gemma)")
            rc = subprocess.run([sys.executable, "-I", "-B", os.path.abspath(__file__), "--build-embeddings", path,
                                 "--transformer", paths["transformer"], "--text-encoder", paths["text_encoder"],
                                 "--video-vae", paths["video_vae"]], stdin=subprocess.DEVNULL).returncode
            if rc != 0 or not os.path.exists(path):
                raise RuntimeError(f"building the prompt embedding cache failed (exit {rc})")
        t = load_file(path, device=self.accel.kind)
        emit("info", f"prompt embedding loaded from {path}")
        return EmbeddingsProcessorOutput(t["video_encoding"], t.get("audio_encoding"), t["attention_mask"])

    def vram_gb(self) -> float:
        return self.accel.reserved_gb()

    def run(self, inp: str, chunks, out_prefix: str, conv: colour.Converter, emit, seed: int = SEED_DEFAULT,
            holds=None):
        torch, accel = self.torch, self.accel
        emit("info", conv.describe())
        reserved_start = accel.reserved_gb()
        self.job_peak = [0.0, 0.0]  # allocated, reserved: max over the chunks (peaks reset per chunk)
        min_free = [accel.free_gb()]
        sampling = threading.Event()

        def sample():
            while not sampling.wait(0.25):
                min_free[0] = min(min_free[0], accel.sample())

        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        try:
            self._run_chunks(inp, chunks, out_prefix, conv, emit, seed, min_free, holds or [1] * len(chunks))
        finally:
            sampling.set()
            sampler.join()
            self._on_step = lambda k, n: None
            with contextlib.suppress(Exception):  # also after an OOM: that is the most useful sample
                emit("vram", f"{min_free[0]:.2f}", f"{self.job_peak[0]:.2f}", f"{self.job_peak[1]:.2f}",
                     f"{reserved_start:.2f}", torch.__version__)

    def _run_chunks(self, inp, chunks, out_prefix, conv, emit, seed, min_free, holds) -> None:  # noqa: ANN001
        torch, accel = self.torch, self.accel
        for i, (start, length) in enumerate(chunks):
            hold = int(holds[i])
            label = f"chunk {i + 1}/{len(chunks)}"
            t0 = time.time()
            accel.reset_peaks()
            alloc_start = accel.allocated_gb()
            emit("stage", i, "converting" if conv.active else "preparing")
            frames = conv.apply(transport.read_frames_range(args_safe(inp), start, length))  # (n,h,w,3) f32
            n, h, w, _ = frames.shape
            if hold > 1:  # decision 74: frame 0 once, then each frame `hold` times
                frames = frames[padding.hold_indices(n, hold)]
            padded, (top, left) = padding.pad_spatial(frames)
            padded = padding.pad_temporal(padded)
            np_, hp, wp = padded.shape[0], padded.shape[1], padded.shape[2]
            frames_t = torch.from_numpy(np.ascontiguousarray(padded))
            if self.frames_on_gpu:
                frames_t = frames_t.to(accel.device)
            MemoryFrameSource.frames = frames_t.clamp_(0.0, 1.0).mul_(255.0)
            emit("info", f"{label}: in {n}x{w}x{h}{f' held {hold}x' if hold > 1 else ''} -> model {np_}x{wp}x{hp} "
                         f"(input ready in {time.time() - t0:.1f} s)")
            emit("stage", i, "denoising")

            def on_step(k: int, steps: int, i: int = i) -> None:
                emit("step", i, k, steps)
                if k == steps:
                    emit("stage", i, "decoding")

            self._on_step = on_step
            with torch.inference_mode():
                # stream_chunks, not __call__: __call__'s pipeline_output_from_chunks insists on audio,
                # and audio decoding is disabled (injection 4).
                decoded, _, _ = self.pipeline.stream_chunks(
                    prompt="", seed=seed, height=hp * 2, width=wp * 2, num_frames=np_, frame_rate=FRAME_RATE,
                    images=[], video_conditioning=[(MEMORY_REF, 1.0)], skip_stage_2=True,
                    vae_dtype=torch.bfloat16 if self.fp16 else None)
                video = torch.cat([c.video.to("cpu") for c in decoded], dim=0)  # (F,H,W,C) bf16 in [0,1]
            MemoryFrameSource.frames = None
            v = video.float()
            if video.dtype == torch.uint8 or float(v.max()) > 1.5:
                v = v / 255.0
            matte = v.mean(dim=-1).clamp_(0.0, 1.0).numpy()  # grayscale matte: average channels
            if matte.shape[1:] != (hp, wp):
                raise RuntimeError(f"decoded size {matte.shape[1:]} != model size {(hp, wp)}")
            if hold > 1:
                matte = matte[padding.held_output_indices(n, hold)]
            transport.write_mattes(f"{out_prefix}_{i}.bin", padding.crop(matte, n, top, left, h, w))
            peak_a, peak_r = accel.peaks_gb()
            self.job_peak = [max(self.job_peak[0], peak_a), max(self.job_peak[1], peak_r)]
            emit("info", f"{label}: done in {time.time() - t0:.1f} s, allocated at start {alloc_start:.1f} GB, "
                         f"peak allocated {peak_a:.1f} GB, peak reserved {peak_r:.1f} GB, allocated now "
                         f"{accel.allocated_gb():.1f} GB, reserved now {self.vram_gb():.1f} GB, "
                         f"lowest free {min_free[0]:.1f} GB")
            emit("chunk_done", i)


def args_safe(path: str) -> str:
    # Staged by the plugin in its staging folder (decision 64: /dev/shm on Linux).
    if not os.path.basename(path).startswith("ltxag_"):
        raise ValueError(f"input must be a staged ltxag_* file, got {path}")
    return path


def classify(exc: BaseException) -> tuple[str, str]:
    import torch

    if isinstance(exc, torch.OutOfMemoryError):  # the same class as torch.cuda.OutOfMemoryError
        return "oom", f"out of GPU memory: {str(exc).splitlines()[0]}"
    if isinstance(exc, FileNotFoundError):
        return "input", str(exc)
    if isinstance(exc, ValueError) and "Cannot fit a DiffVAE decode tile" in str(exc):  # decision 72
        return "too_large", ("frame too large for the LTX decoder: scale the clip down before the node "
                             "and the matte up after it, or limit the region of interest with an alpha")
    first = str(exc).splitlines()[0] if str(exc) else ""
    return "internal", f"{type(exc).__name__}: {first}"


# --------------------------------------------------------------------------- one-shot mode
def one_shot(a) -> int:  # noqa: ANN001
    def emit(*parts):
        sys.stdout.write(" ".join(str(p) for p in parts) + "\n")
        sys.stdout.flush()

    threading.Thread(target=lambda: (sys.stdin.read(), os._exit(3)), daemon=True).start()
    t0 = time.time()
    emit("info", f"python={sys.executable} {sys.version.split()[0]} pid={os.getpid()}")
    try:
        conv = colour.Converter(a.colour, a.ocio_config, a.colour_tag)
        engine = Engine(a.model_dir, a.embeddings, a.precision, emit, keep_transformer=not a.rebuild_transformer,
                        frames_on_gpu=a.frames_on_gpu)
        engine.run(a.inp, [tuple(int(v) for v in c.split(":")) for c in a.chunks.split(",")],
                   a.out_prefix, conv, emit, a.seed,
                   [int(k) for k in a.holds.split(",")] if a.holds else [a.hold] * len(a.chunks.split(",")))
    except Exception as exc:  # noqa: BLE001 - report everything, with traceback
        kind, msg = classify(exc)
        emit("error", kind, msg)
        traceback.print_exc(file=sys.stdout)
        return 2
    emit("info", f"job finished in {time.time() - t0:.1f} s")
    emit("done")
    return 0


# --------------------------------------------------------------------------- serve mode
class Server:
    def __init__(self, a) -> None:  # noqa: ANN001
        self.a = a
        self.sock_path, self.info_path = a.serve, a.serve + ".json"
        self.idle = float(os.environ.get("LTXAG_IDLE_TIMEOUT", a.idle_timeout))
        self.engine = None

    def write_info(self, state: str, message: str = "") -> None:
        info = {"pid": os.getpid(), "precision": self.a.precision, "model_dir": self.a.model_dir,
                "state": state, "message": message, "time": time.time(),
                "vram_gb": round(self.engine.vram_gb(), 2) if self.engine else 0.0}
        tmp = f"{self.info_path}.tmp{os.getpid()}"
        with open(tmp, "w") as f:
            json.dump(info, f)
        os.replace(tmp, self.info_path)

    def flame_alive(self) -> bool:
        pid = self.a.flame_pid
        if not pid:
            return True
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def serve(self) -> int:
        os.makedirs(os.path.dirname(self.sock_path), exist_ok=True)
        log.info(f"server starting: precision={self.a.precision} model_dir={self.a.model_dir} "
                 f"idle_timeout={self.idle:.0f}s flame_pid={self.a.flame_pid}")
        self.write_info("loading")
        try:
            self.engine = Engine(self.a.model_dir, self.a.embeddings, self.a.precision,
                                 lambda *p: log.info(" ".join(str(x) for x in p)),
                                 keep_transformer=not self.a.rebuild_transformer,
                                 frames_on_gpu=self.a.frames_on_gpu)
        except Exception as exc:  # noqa: BLE001
            kind, msg = classify(exc)
            log.error(f"server failed to load: {kind} {msg}\n{traceback.format_exc()}")
            self.write_info("failed", f"{kind} {msg}")
            return 2
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.sock_path)
        os.chmod(self.sock_path, 0o600)
        srv.listen(8)
        srv.settimeout(1.0)
        self.write_info("ready")
        log.info(f"server ready on {self.sock_path}, reserved {self.engine.vram_gb():.1f} GB")
        last = time.time()
        reason = ""
        try:
            while True:
                if not self.flame_alive():
                    reason = f"Flame (pid {self.a.flame_pid}) is gone"
                    break
                if time.time() - last > self.idle:
                    reason = f"idle for {self.idle:.0f} s"
                    break
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    continue
                with conn:
                    outcome = self.handle(conn)
                last = time.time()
                if outcome in ("unload", "restart"):
                    reason = "unload requested" if outcome == "unload" else "restart for other settings"
                    break
        finally:
            srv.close()
            for p in (self.sock_path, self.info_path):
                try:
                    os.unlink(p)
                except FileNotFoundError:
                    pass
            log.info(f"server exiting: {reason}")
        return 0

    def handle(self, conn: socket.socket) -> str:
        conn.settimeout(None)
        f = conn.makefile("rwb", buffering=0)

        def emit(*parts):
            try:
                f.write((" ".join(str(p) for p in parts) + "\n").encode())
            except (BrokenPipeError, ConnectionResetError, OSError) as exc:
                raise Cancelled() from exc

        try:
            req = json.loads(f.readline().decode() or "{}")
        except json.JSONDecodeError as exc:
            emit("error", "input", f"bad request: {exc}")
            return "ok"
        if req.get("ping"):
            emit("pong", self.a.precision, self.a.model_dir)
            return "ok"
        if req.get("precision") != self.a.precision or req.get("model_dir") != self.a.model_dir:
            log.info(f"request needs precision={req.get('precision')} model_dir={req.get('model_dir')}; restarting")
            emit("restart")
            return "restart"
        self.write_info("busy")
        log.job = req.get("job_id", "-")
        t0 = time.time()
        log.info(f"job start (resident engine reused): chunks={req['chunks']} colour={req.get('colour')} tag={req.get('colour_tag', '')!r} "
                 f"unload={req.get('unload')} holds={req.get('holds')}")
        try:
            conv = colour.Converter(req.get("colour", "Bypass"), req.get("ocio_config", ""), req.get("colour_tag", ""))
            self.engine.run(req["in"], [tuple(c) for c in req["chunks"]], req["out_prefix"], conv, emit,
                            int(req.get("seed", SEED_DEFAULT)), req.get("holds"))
            emit("done")
            log.info(f"job done in {time.time() - t0:.1f} s")
        except Cancelled:
            log.warn("client disconnected mid-job; job abandoned")
        except Exception as exc:  # noqa: BLE001
            kind, msg = classify(exc)
            log.error(f"job failed: {kind} {msg}\n{traceback.format_exc()}")
            try:
                emit("error", kind, msg)
            except Cancelled:
                pass
            if kind == "oom":
                self.engine.drop_transformer()
        finally:
            MemoryFrameSource.frames = None
            log.job = "-"
            self.write_info("ready")
        return "unload" if req.get("unload", True) else "ok"


def main() -> int:
    if "--fp16-probe" in sys.argv:  # decision 68: Setup asks whether fp16 is faster here
        import torch

        bf16_ms, fp16_ms = fp16.probe(torch, Accel(torch).device)
        print(f"fp16_probe {bf16_ms:.2f} {fp16_ms:.2f}", flush=True)
        return 0
    if "--fp16-convert" in sys.argv:
        import torch

        i = sys.argv.index("--fp16-convert")
        t0 = time.time()
        zeros, nonzero = fp16.convert(torch, sys.argv[i + 1], sys.argv[i + 2], lambda m: print(m, flush=True))
        print(f"fp16 copy written in {time.time() - t0:.0f} s; {zeros} of {nonzero} nonzero weights "
              f"became 0 ({100.0 * zeros / max(1, nonzero):.5f} %)", flush=True)
        return 0
    if "--build-embeddings" in sys.argv:
        bp = argparse.ArgumentParser()
        bp.add_argument("--build-embeddings", required=True)
        bp.add_argument("--transformer", required=True)
        bp.add_argument("--text-encoder", required=True)
        bp.add_argument("--video-vae", required=True)
        b = bp.parse_args()
        return build_embeddings(b.build_embeddings, b.transformer, b.text_encoder, b.video_vae)
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", default=None, help="Unix socket path: run as resident server")
    ap.add_argument("--in", dest="inp", default=None)
    ap.add_argument("--chunks", default=None)
    ap.add_argument("--out-prefix", default=None)
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--embeddings", required=True)
    ap.add_argument("--precision", default="bf16", choices=["bf16", "fp8", "fp16"])
    ap.add_argument("--colour", default="Bypass", choices=[colour.AUTO] + list(colour.SOURCES))
    ap.add_argument("--colour-tag", default="", help="with --colour Auto: the colourspace the frames are in")
    ap.add_argument("--ocio-config", default="")
    ap.add_argument("--seed", type=int, default=SEED_DEFAULT)
    ap.add_argument("--hold", type=int, default=1, choices=[1, 2, 4, 8], help="Frame Hold for every chunk (74)")
    ap.add_argument("--holds", default=None, help="Frame Hold per chunk, e.g. 1,4,1 (75)")
    ap.add_argument("--flame-pid", type=int, default=0)
    ap.add_argument("--idle-timeout", type=float, default=IDLE_TIMEOUT_DEFAULT)
    ap.add_argument("--rebuild-transformer", action="store_true", help="A/B: upstream behaviour, rebuild per chunk")
    ap.add_argument("--frames-on-gpu", action="store_true", help="A/B: whole chunk on the GPU (pre-2026-10-11)")
    ap.add_argument("--profile", action="store_true", help="timestamped LTX library logging to stdout")
    ap.add_argument("--seconds", default=None, help="ignored (dummy worker option)")
    ap.add_argument("--simulate", default="none", help="ignored (dummy worker option)")
    a = ap.parse_args()
    if a.profile:
        import logging

        logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                            format="profile %(asctime)s.%(msecs)03d %(name)s: %(message)s", datefmt="%H:%M:%S")
    if a.serve:
        return Server(a).serve()
    if not (a.inp and a.chunks and a.out_prefix):
        ap.error("one-shot mode needs --in, --chunks and --out-prefix")
    return one_shot(a)


if __name__ == "__main__":
    sys.exit(main())
