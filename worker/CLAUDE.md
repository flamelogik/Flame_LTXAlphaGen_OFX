# worker/ — job controller and workers

Part of the design notes (root `CLAUDE.md` has the overview and the decision index).

## Files

- `job.py`: the detached job controller (resident/subprocess channels, progress Tracker, VRAM
  record, refresh marker, merge, ROI paste-back). It takes the staging folder from its manifest's
  folder, so only the plugin knows it. Manifest: `job_id node_id input width height frames chunks
  contrib frame_keys cache_root worker [worker_python worker_args worker_socket model_dir unload
  colour ocio_config seed] seconds simulate precision vram flame_pid title`, plus `merge`
  ("smooth"|"flow"), `full_width full_height`, `roi` [x,y,w,h] (with an ROI, `width/height` = crop).
- `ltx_worker.py`: the real worker. `dummy_worker.py`: a luminance "matte" with `--simulate
  oom|crash|hang`, same line protocol.
- `ltxag_worker/{padding,transport,cache,log,colour,morph,oslayer,fp16}.py`,
  `ltxag_worker/mac_progress.swift`.

## Model and runtime facts

- **Alpha Gen** = IC-LoRA `ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors` on LTX-2.5-22B (gated
  HF repos `Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen`, `Lightricks/LTX-2.5`).
- Limits: dimensions /32, ≤145 frames of the form 8n+1 (longer clips bleed RGB into the matte).
  The card says "≤1920×1088", but larger sizes run (decision 26). Empty prompt; recipe 1 = 8
  distilled steps.
- **Required files** (~73 GB): `diffusion_models/ltx-2.5-22b-distilled-transformer-bf16` (42.0 GB),
  `text_encoders/gemma4-12b-with-proj-ltx-2.5-bf16` (26.3 GB), `vae/ltx-2.5-video-vae-bf16`
  (1.47 GB), `vae/ltx-2.5-audio-vae-bf16` (0.36 GB),
  `latent_upscale_models/ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0` (1.0 GB),
  `loras/ltx-2.5-22b-ic-lora-alpha-gen-0.9` (1.31 GB). `ICLoraPipeline` demands the audio VAE and
  the upscaler even with `skip_stage_2`.
- **Stack:** LTX-2 pinned at commit **`9ec55f9`**, `uv sync --frozen --no-dev --extra natten` with
  our lockfile → Python 3.14.7, torch 2.13.0+cu132 (macOS: 2.14.1). The code is under the LTX
  licence ("inference-enabling code" §1.9), so it's never bundled. LTX-2 citations in the design
  notes and the code refer to that commit.
- **Licensing:** entities with ≥ USD 10M revenue need a paid Lightricks licence (§2.1; Setup tab
  and README say so); the full `LICENSE-2_x` ships as `helper/licence.txt`; the README mentions
  AI-content disclosure. No separate legal read (closed by the user, 2026-10-14).

## Images (decisions 8/23/27/28/54, 24)

- **76.** Input colour is Auto / Bypass (the old dropdown is hidden, kept for saved setups). Auto: the plugin asks
  Flame for ACEScg; the worker gets the delivered tag (`colour_tag`, `--colour-tag`) and converts by it: no tag,
  Rec.1886 Rec.709 or any other display/data space (sRGB, Matte, Raw) = no conversion (PROPOSED: a P3/Rec.2020
  display clip is not gamut-converted); a scene-referenced name = the OCIO source; an unknown name fails the job.
  The fixed view stays `ACES 2.0 - SDR 100 nits (Rec.709)` on display `Rec.1886 Rec.709 - Display`, a
  `DisplayViewTransform` (a plain `getProcessor(src, display)` skips the tone mapping). The worker uses its own
  config (`$OCIO`, else Flame's newest ACES 2.0), not the host's.
- **54 (supersedes 14):** conversion runs here with PyOpenColorIO 2.6.0, not in the C++ shim (the
  fixed dropdown removed the reason; no OCIO build, no clash with Flame's OCIO). Frames travel as
  float16 in the source colour space. `applyRGB` releases the GIL: one frame per thread (16),
  820 → 83 ms per 2K frame, identical output.
- **24.** Padding to /32 (pixels) and 8n+1 (frames) is automatic and invisible; the output matches
  the source size and frame count.

## Worker (`ltx_worker.py`)

- An `Engine` (loaded once per process) drives the official `ICLoraPipeline` through
  **`stream_chunks`**, not `__call__` (its `pipeline_output_from_chunks` insists on audio).
  Settings: `skip_stage_2`, 2× width/height so stage 1 is native, seed 1234, LoRA 1.0 with
  `LTXV_LORA_COMFY_RENAMING_MAP`, fp8 via `QuantizationKind("fp8-cast").to_policy(transformer)`.
  Injections:
  - `ic_lora.SequentialVideoFrameSource` → `MemoryFrameSource` (frames from staging as floats
    0–255, **one frame to the GPU per `take()` step**, as upstream's decoder does: −3.4 GB at HD
    145, identical mattes; `--frames-on-gpu` keeps the old way for A/B only);
  - `pipeline.prompt_encoder` → `CachedPromptEncoder`; `pipeline.audio_decoder = None`
    (video-only, `chunks/operators.py:285`);
  - **55:** with Unload off, **class-level** `DiffusionStage._transformer_ctx` → build once, keyed
    by checkpoint + LoRA set (saves ~6 s per chunk, but holds a transformer through the next
    encode and decode, so with Unload on the worker rebuilds per chunk as upstream does). The
    single-GPU `Builder.build` ignores the per-call `video_tools` kwarg (ARG002);
  - `ltx_pipelines.utils.samplers.tqdm` → a stepping wrapper that reports each step (no callback;
    `samplers.py:72`);
  - MPS only: `pipeline.image_conditioner` → `ReferenceEncoderInDtype` (below).
- Device: CUDA or MPS through `Accel` (memory queries, peaks sampled on MPS, OOM type
  `torch.OutOfMemoryError` for both). Modes: one-shot (`--in/--chunks/--out-prefix`, smoke tools),
  `--serve SOCKET`, `--build-embeddings`, `--fp16-probe`, `--fp16-convert`.
- Colour via `colour.Converter` before padding. `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True`
  before torch loads. Decoded output `(F,H,W,3)` bf16 in [0,1]; matte = mean of the channels.
- **Per-chunk time** (HD 65 frames fp8, `--profile`): reference encode 1.9 s, transformer build
  6.6 s (when rebuilt), denoise 60.2 s (8 steps), decode 10.5 s.
- **9. Prompt cache:** the *final* `EmbeddingsProcessorOutput` (`video_encoding` (1,1024,4096) bf16
  + `attention_mask`), keyed by the transformer + Gemma file names and sizes (one transformer
  file; fp8-cast doesn't touch the embeddings processor). **Built in a child process**
  (`--build-embeddings`): in-process, Gemma's memory wasn't freed (OOM at 94 vs 45.5 GB).
- Errors: `OutOfMemoryError` → `error oom`; else `error internal` + traceback in the log. Peak
  stats are reset per chunk; each chunk logs allocated-at-start and its peaks.
- **56, the VRAM record:** the worker samples the lowest free memory every 0.25 s (`cudaMemGetInfo`) and reports
  `vram <lowest free> <torch peak allocated> <peak reserved> <held at start> <torch version>`;
  job.py logs one line and appends a row to `logs/vram_stats.csv` (in Collect Logs; no
  telemetry). used = preflight free + resident − lowest free. Runs without preflight numbers
  (smoke tools) are logged but not recorded.
- **Line protocol** (the dummy speaks it too): `info <text>`, `progress <f> <text>`, `stage <chunk>
  converting|preparing|denoising|decoding`, `step <chunk> <k> <n>`, `chunk_done <i>`, `vram …`,
  `done`, `error <kind> <text>`. job.py's `Tracker` shows "Chunk i/n · frames a–b of N · step k/8 ·
  m:ss elapsed · about m:ss left", redrawn every second; time left starts after step 2 (step 1
  includes the build; decode ≈ 1.4 steps until a chunk has finished). Verified in Flame. No frame
  counter: all frames of a chunk are denoised together.
- **Verified:** `worker_smoke` HD 60 bf16 error 0.0029 vs truth; `job_real_smoke` (200 frames, 2
  chunks) max error 0.0026; good mattes on real footage in Flame.
- **72. No 6K (won't do; user, 2026-10-14).** 6144×3264 fails in ~12 s, before inference: the
  DiffVAE decode planner (`ic_lora.py:486` → `diffusion_tiling.py:402`) holds whole-clip stage-4
  features (× 49: 42.4 of 55.3 GB usable; × 25: 27.0 of 70.7) and finds no tile (min 80f × 320²).
  The transformer would also be slow (× 49 ≈ 274k tokens, ~4–5 min/step on the Blackwell, est.).
  Users scale down and up outside the node, or crop with the ROI (63). Process refuses a long
  side ≥ 6000 px (`ofx/CLAUDE.md` 26); `classify` turns the planner's refusal into `too_large`. Unexplored: the conv VAE
  `ltx-2.5-video-vae-conv-bf16` (1.45 GB), short 6K chunks, `--diffvae-optimization`, `--tile`.

## Frame Hold (decision 74, DECIDED, user 2026-10-17)

- **Why:** the video VAE folds 8 frames into one latent (32×32×8, frame 0 alone: `F' = 1 +
  (F−1)/8`, `video_vae.py:168`, space-to-depth in `sampling.py:39-60`); the Alpha Gen LoRA keeps
  reference and output latents 1:1 (metadata: `reference_downscale_factor 1`, no
  `reference_temporal_scale_factor`, `iclora_utils.py:43`); the diffusion VAE decoder rebuilds 8
  matte frames per latent. Fast motion within 8 frames → soft, lagging, jittering edges (a
  user's report; their workaround: repeat each frame 8×, drop the copies afterwards).
- **How:** after colour conversion, `padding.hold_indices` keeps frame 0 once and repeats every
  later frame `hold` (2/4/8) times; after decoding `held_output_indices` keeps the middle copy.
  Per chunk (75): `"holds"` in the request (resident, no restart), `--holds 1,4,1` or `--hold K`
  (one-shot). job.py's Tracker weights chunks by model frames (1 + hold × (len − 1)), so the ETA
  holds with Auto's mixed chunks, and shows "4x hold" in the progress line. `FRAME_RATE` only feeds
  the timing embedding, not the grouping: no lever.
- **A/B** (`tools/hold_ab.py`, HD bf16 Blackwell, 19 frames, textured fringed disc on a circle,
  180° shutter blur, truth known): edge-band error hold 1/2/4/8 at 24 px/frame 0.022/0.016/0.014/
  0.017, 64: 0.047/0.031/0.020/0.019, 128: 0.054/0.029/0.023/0.021; worst centroid lag 1→4×:
  7.3→2.6, 12.1→3.9, 26.4→4.7 px (mean lag ~1 px everywhere: jitter, not offset); 4 px/frame:
  no difference. **4× is the sweet spot**; 8× barely better. Time per 145-model-frame chunk:
  45/89/230 s (hold 1 with 19 frames: 33 s). Pick offset: within 0.001 (middle kept).
  `--hold 8` = external expansion in quality (edge 0.0214 vs 0.0213). Synthetic only: real
  footage (hair) UNVERIFIED.
- **Determinism:** the same chunk run first in two processes is bit-identical; the same input
  run as the 4th chunk of a process differed (max 0.21, mean 0.0008). Likely state carried
  between chunks of one process (resident worker too); cause UNVERIFIED, pre-existing.

## Resident worker (decisions 11/19/33/55)

- `ltx_worker.py --serve ~/.cache/ltx-alphagen/run/worker.sock`, plus `worker.sock.json` `{pid,
  precision, model_dir, state loading|ready|busy|failed, vram_gb}`; stdout/stderr to
  `run/worker.out`. One job per connection, sequentially. Another precision or model_dir → `restart`
  (it exits; the client starts a matching one).
- Exits after a job with `unload: true`, after **5 min idle** (33; `$LTXAG_IDLE_TIMEOUT` for
  tests), or when `flame_pid` dies.
- job.py's `ResidentChannel` starts the server (detached, own session) under an `flock`, connects,
  streams the same lines; Cancel = `killpg(server)`; a server it started is checked through its
  Popen. The dummy keeps `SubprocessChannel`. The socket closes before a killed server has died,
  so job.py waits up to 5 s before classifying.
- `tools/resident_smoke.py` (15 checks): stay, reuse (22 → 11 s), precision change, unload,
  idle timeout, Flame death, mid-job kill ("ended unexpectedly (signal 9)", no leftovers), ACEScg.

## Merge (decision 62, Flow Warp)

`ltxag_worker/morph.py`, **numpy only** (job.py runs under the system python3 on Linux; no
OpenCV): Horn-Schunck coarse to fine on a copy ≤ 512 px, one flow each way, then
`a_t = a(x + t·F_ba)`, `b_t = b(x + (1−t)·F_ab)`, `out = (1−t)·a_t + t·b_t` (exact at t = 0 and 1),
clamped to 5% displacement, a plain blend if the mattes differ by < 0.01. Synthetic discs 6–40 px apart:
error 8–10× lower than a dissolve, 0.36 s per HD frame (~9 s per 24-frame seam). Only on blended
frames, in job.py's `finalize`. Doesn't fix a switch of the main object. Open: an ML morph, or
conditioning the next chunk on the previous matte.

## Python OS layer (decision 64)

`ltxag_worker/oslayer.py` (progress window with Cancel, message windows, `process_name`), shared
by job.py and setup_helper.py. Linux: zenity and /proc. **macOS:** the progress window is
`mac_progress.swift`, an AppKit window reading zenity's stdin protocol, compiled once per source
version with `xcrun swiftc` into `~/.cache/ltx-alphagen/bin` (first build ~1 min, then cached).
Rejected on macOS 15: osascript's `progress` draws nothing, the system Tk 8.5 aborts at `Tk()`,
JXA Cocoa needs the untyped ObjC bridge. Messages: osascript `display dialog` (text as argv).
`process_name`: basename of `ps -o comm=` (Flame's is `.../flame.app/Contents/MacOS/flame` →
"flame"). Checked over ssh: builds, "# text"/42/100 and end of input exit 0, window on screen.
Not checked: Cancel / close button (exit 1), the look. On macOS job.py runs under `ltx_python`
(all worker tests pass under it); `test_job` stages in the temp folder off Linux.

## macOS: the MPS worker (decision 65)

- Upstream LTX-2 supports Apple Silicon (`mps-sdpa` attention, an MPS LoRA-fuse path, eager
  neighbourhood attention without natten) and our lockfile resolves for macOS: same `ICLoraPipeline`.
- **`tools/mac_bench.py`** (TFLOPS, PyTorch / MLX): M1 Ultra matmul fp16 17.5 / 16.5, bf16 8.8 / 14.7.
  M5 Ultra 80c (2026-10-14): within 3 % in fp16 = bf16 (matmul ~115, attention ~90), so PyTorch
  reaches the Neural Accelerators in bf16; MLX leads only in fp32 (~75 vs 28). No speed case for MLX.
- **Measured on the M1 Ultra** (128 GB, 96 GB Metal working set, torch 2.14.1, `worker_smoke`,
  bf16, 2026-10-12):

  | Clip | Encode | Steps (8) | Decode | Job | Peak held | Matte error |
  |---|---|---|---|---|---|---|
  | 720p × 49 | 126 s | 440 s (55 s/step) | ~69 s | 674 s | 52.3 GB | 0.0027 |
  | HD × 33 | 188 s | 776 s (97 s/step) | ~105 s | 1112 s | 60.4 GB | 0.0033 |
  | HD × 73 | 406 s | 2120 s (265 s/step) | ~220 s | 2861 s | 79.7 GB | 0.0030 |

  ~50 s per new HD frame; HD × 145 would need ~115 GB and ~2.2 h. CUDA: HD × 60 in 85 s.
- **HD × 73 estimates** (UNVERIFIED, 2026-10-14). Steps scale by `mac_bench` matmul; M2/M3 = M1's 83 %
  of shader peak (Apple spec, recalled: 21/27/28 TFLOPS), bf16; encode/decode by shader peak (upper
  bound, no conv3d data). Blackwell: HD × 65 profile scaled; runs HD × 49/97/145 took 73/143/240 s.

  | Machine | Precision | Encode | Steps (8) | Decode | Job |
  |---|---|---|---|---|---|
  | M1 Ultra 64c | fp16 | 406 s | ~1510 s | ~220 s | ~38 min |
  | M2 Ultra 76c | bf16 | ~315 s | ~1170 s | ~170 s | ~30 min |
  | M3 Ultra 80c | bf16 | ~300 s | ~1120 s | ~165 s | ~28 min |
  | M5 Ultra 80c | bf16 | ~60–190 s | ~160–240 s | ~30–100 s | ~6–10 min |
  | RTX PRO 6000 Blackwell (Linux) | fp8 or bf16 | ~2 s | ~71 s | ~12 s | ~1.5–2 min |
- **Encode dtype:** M1 has no fast bf16 (conv3d bf16 0.40 TFLOPS vs fp32 0.92, fp16 4.74 but NaN
  in the VAE encoder even with PixelNorm in fp32; matmul bf16 8.9, fp16 16.4, fp32 13.0).
  `pick_encode_dtype` times one Conv3d (128→128, k3, 1×128×5×64×64) in bf16 and fp32 at engine
  start and encodes in the faster (tie → bf16), logged as `reference encode in <dtype> (conv3d
  probe: …)`. M1 Ultra: bf16 43 ms, fp32 20 ms → fp32 in 5 of 5 runs, probe 0.43 s; 285 → 126 s
  at 720p × 49, same matte. An M2+ choosing bf16: UNVERIFIED.
- **Cancel:** the worker dies within 0.2 s of SIGKILL, but **submitted GPU work runs on** (~20 s
  at 100 %). The encode wrapper calls `torch.mps.synchronize()` after every `nn.Conv3d` (forward
  hooks): same latent (max diff 0.0), same time (85.5 vs 86.4 s at 720p × 33), GPU idle ~3 s after
  a kill. job.py waits for the worker and logs "cancel: worker pid … exited X s after SIGKILL".
  Mid-denoise: unmeasured.
- **macOS 26 Tahoe** (M1 Ultra, 26.7.1, torch 2.14.1, 2026-10-18): matmul fp16/bf16 17.4/8.8 TFLOPS
  and attention as on Sequoia; conv3d ~15× faster (probe 2–3 ms, bf16 ≈ fp32). 512² × 33 fp16:
  steps 6.9 s, decode 25 s, chunk 96 s warm / 162 s cold; system test real matte 117 s (Sequoia
  169 s). The first runs right after the upgrade took 2.8× longer (468 s; post-upgrade indexing
  and cache rebuilds, transient). A cold transformer build (42 GB fp16, mmap) takes 77 s vs 12 s
  warm, though the SSD reads 5.7 GB/s cold: the mmap load path, not the disk (Sequoia: 46 s).
- **No fp8 on MPS** (`Undefined type Float8_e4m3fn`, torch's MPS dtype table,
  `aten/src/ATen/native/mps/OperationUtils.mm`): true for every Apple chip.
- **mps-sdpa's compiled bridge** needs `ninja` on PATH and fails on stale CLT headers; its fallback
  is as fast (255 ms per 12.9k-token attention call either way).
- **68. fp16 transformer where faster** (user 2026-10-13, automatic). M1 Ultra 720p × 49, same
  seed: no inf/NaN in 384 block outputs, largest activation 13,760 (fp16 max 65,504; blocks
  40–43); weights: none beyond fp16's range, 0.0003 % of nonzero ones become 0; matte error 0.0029
  both; fp16 vs bf16 mean difference 0.00024 (99th percentile 0.0015); steps 54.6 → 38.9 s (1.40×),
  job 692 → 583 s (1.19×). Casting at load (`Builder.build(dtype=fp16)`) held ~26 GB more (78.5 vs
  52.7 GB) and loaded slower; an fp16 copy loads like the bf16 file (52.3 GB both), so Setup
  writes an **fp16 copy** (42 GB, `helper/CLAUDE.md`). Worker precision "fp16" = the bf16 pipeline
  with transformer, denoising state, prompt embedding and reference latent in fp16, VAE decode in
  bf16; a missing copy is an error (34). `fp16.py`: `probe` (4096² matmul bf16 vs fp16) and
  `convert` (streaming BF16 → F16, F32 tables and metadata kept). Per Mac, not per chip: M2–M4
  have native bf16 (UNVERIFIED by us), M5 runs bf16 as fast as fp16 (`mac_bench` above): no copy.

## Dead ends: don't retry

- `ICLoraPipeline.__call__` with `audio_decoder=None`: "expected decoded audio"; use `stream_chunks`.
- `ModelRegistry(cache_weights=True)`: chunk 1 200 s instead of 80, +16 GB; use `_transformer_ctx`.
- Patching `_transformer_ctx` on the **instance**: `with_loras()` copies the stage, so the copy
  built the base model **without the IC-LoRA** (the matte reproduced the picture). Class level.
- Building the Gemma cache in the worker process: its memory stays held → OOM.
- `kill(pid, 0)` on a killed child: zombies answer; use `Popen.poll()`.
- Single-threaded `applyRGB` on a whole chunk (0.8 s per 2K frame); a chunk's frames held on the
  GPU as float32 (3.4 GB wasted at HD 145).
