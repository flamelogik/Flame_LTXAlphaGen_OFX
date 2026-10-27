"""Background processing job (decision 35). Launched detached by the OFX Process button.

  python -I -B job.py <manifest.json> [--no-ui]

The shim has already staged every source frame in its staging folder (the manifest's folder,
/dev/shm on Linux; decision 64) and computed each frame's
cache key and crossfade contributions. This process, outside Flame:
  - shows its own progress window with a Cancel button (ltxag_worker/oslayer.py),
  - runs the worker once for all chunks (the real model will load once per job),
  - blends overlapping chunks and writes one matte per frame into the disk cache,
  - always cleans up its staged files and pending markers, and records a final status.
Manifest fields: job_id node_id input width height frames chunks[[start,len]] frame_keys[]
contrib[[[chunk,weight],...]] cache_root worker [worker_python] [worker_args[]] seconds simulate
precision title. ROI (decision 63): width/height are then the CROP staged in `input`, plus
full_width full_height roi[x0,y0,w,h] (top-down); results are pasted into black full frames.
Merge (decision 62): merge "smooth" blends overlaps by the weights in contrib; "flow" warps the
two mattes towards each other first (ltxag_worker/morph.py).
Status file <cache_root>/jobs/<job_id>.json is rewritten after every chunk (state "running")
and at the end, so the plugin can follow progress.
Real worker (manifest has worker_python): talks to the RESIDENT worker server over a Unix socket
(ltx_worker.py --serve; decisions 11/19/33/55), starting it if none is running. Extra manifest
fields: worker_socket, unload, flame_pid, colour ("Auto"|"Bypass"), colour_tag (the colourspace the
host delivered; absent = ""), ocio_config, seed. holds[] (one per chunk, decisions
74/75): the Frame Hold the worker applies; the progress weights chunks by model frames. Dummy worker: one subprocess
per job, as before.
Progress window (user request 2026-10-11): the worker's "stage"/"step" lines drive a Tracker;
a 1 s ticker redraws "Chunk i/n · frames a–b of N · step k/8 · m:ss elapsed · about m:ss left",
so the elapsed time proves the job is alive even in stages without steps. The time left appears
once two denoising steps are timed (step 1 includes the transformer build).
VRAM log (decision 56): the manifest's "vram" (the plugin's preflight numbers) plus the worker's
"vram" line give estimate vs actual; one line in the log and one row in logs/vram_stats.csv.
"""

from __future__ import annotations

import csv
import fcntl
import json
import os
import queue
import signal
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

from ltxag_worker import cache, morph, oslayer, transport  # noqa: E402
from ltxag_worker.log import Logger, log_path  # noqa: E402

log = Logger("job")


class Progress:
    """The progress window; cancelled() is True once the user pressed Cancel."""

    def __init__(self, title: str, enabled: bool) -> None:
        self.win = oslayer.ProgressWindow(title, 560, enabled)
        self.finished = False
        self.lock = threading.Lock()  # the main loop and the 1 s ticker both write

    def update(self, frac: float, text: str) -> None:
        with self.lock:
            if not self.win.shown or self.win.gone() or self.finished:
                return
            try:
                self.win.show(frac, text)
            except BrokenPipeError:
                pass

    def cancelled(self) -> bool:
        return not self.finished and self.win.gone()

    def close(self) -> None:
        with self.lock:
            self.finished = True
        self.win.close(wait=5)

    @staticmethod
    def error(title: str, text: str, enabled: bool) -> None:
        oslayer.message("error", title, text, 420, enabled)


def mmss(seconds: float) -> str:
    seconds = max(0, int(round(seconds)))
    h, rest = divmod(seconds, 3600)
    return f"{h}:{rest // 60:02d}:{rest % 60:02d}" if h else f"{rest // 60}:{rest % 60:02d}"


class Tracker:
    """Turns worker events into the progress line, fraction and time-left estimate."""

    DECODE_PER_STEP = 1.4  # decode time / one step time before a chunk has finished (HD profile: 10.5 / 7.5 s)

    def __init__(self, chunks: list, frames: int, holds: list | None = None) -> None:
        self.chunks, self.frames = chunks, frames
        self.holds = list(holds) if holds else [1] * len(chunks)
        # A chunk costs its model frames: with a Frame Hold (74/75) a short chunk can cost as much
        # as a long one.
        self.cost = [1 + k * (n - 1) for (_, n), k in zip(chunks, self.holds)]
        self.total = float(sum(self.cost))
        self.t0 = time.time()
        self.lock = threading.Lock()
        self.status = "Starting the worker"
        self.chunk = -1
        self.stage = ""
        self.step, self.steps = 0, 0
        self.chunk_t0 = self.denoise_t0 = self.last_step_t = 0.0
        self.step_times: list[float] = []  # step 2+ only (step 1 includes the transformer build)
        self.pre = self.post = None  # measured seconds before the first step / after the last
        self.done_chunks = 0

    def note(self, text: str) -> None:
        with self.lock:
            if self.chunk < 0:
                self.status = text

    def on_stage(self, i: int, name: str) -> None:
        now = time.time()
        with self.lock:
            if i != self.chunk:
                self.chunk, self.chunk_t0, self.step, self.steps = i, now, 0, 0
            self.stage = name
            if name == "denoising":
                self.denoise_t0 = self.last_step_t = now
                if self.pre is None or i > 0:
                    self.pre = now - self.chunk_t0

    def on_step(self, i: int, k: int, n: int) -> None:
        now = time.time()
        with self.lock:
            if k >= 2:
                self.step_times.append(now - self.last_step_t)
            self.step, self.steps, self.last_step_t = k, n, now

    def on_chunk_done(self, i: int) -> None:
        now = time.time()
        with self.lock:
            self.done_chunks = i + 1
            if self.steps and self.step == self.steps:
                self.post = now - self.last_step_t
            self.stage = "done"

    def _fraction(self) -> float:
        if self.chunk < 0:
            return 0.0
        inner = {"converting": 0.0, "preparing": 0.0, "decoding": 0.9, "done": 1.0}.get(self.stage, 0.05)
        if self.stage == "denoising" and self.steps:
            inner = 0.05 + 0.85 * self.step / self.steps
        before = sum(self.cost[: self.chunk])
        return (before + inner * self.cost[self.chunk]) / self.total

    def _left(self) -> float | None:
        if not self.step_times or self.chunk < 0:
            return None
        step = sum(self.step_times[-8:]) / len(self.step_times[-8:])
        steps = self.steps or 8
        post = self.post if self.post is not None else self.DECODE_PER_STEP * step
        pre = self.pre or 0.0
        cost_cur = self.cost[self.chunk]
        if self.stage in ("converting", "preparing"):
            cur = pre + steps * step + post
        elif self.stage == "denoising":
            cur = (steps - self.step) * step + post
        elif self.stage == "decoding":
            cur = max(0.0, post - (time.time() - self.last_step_t))
        else:
            cur = 0.0
        later = sum((pre + steps * step + post) * c / cost_cur for c in self.cost[self.chunk + 1:])
        return cur + later

    def render(self) -> tuple[float, str]:
        with self.lock:
            elapsed = f"{mmss(time.time() - self.t0)} elapsed"
            if self.chunk < 0:
                return 0.0, f"{self.status} · {elapsed}"
            start, n = self.chunks[self.chunk]
            parts = [f"Chunk {self.chunk + 1}/{len(self.chunks)}",
                     f"frames {start + 1}–{start + n} of {self.frames}"]
            if self.holds[self.chunk] > 1:
                parts.append(f"{self.holds[self.chunk]}x hold")
            if self.stage == "denoising":
                parts.append(f"step {self.step}/{self.steps}" if self.steps else "starting")
            elif self.stage:
                parts.append(self.stage)
            parts.append(elapsed)
            left = self._left()
            parts.append(f"about {mmss(left)} left" if left is not None else "estimating time left")
            return self._fraction(), " · ".join(parts)


def record_vram(m: dict, line: str, state: str) -> None:
    """Estimate vs actual (decision 56): a log line and a row in logs/vram_stats.csv."""
    est = m.get("vram") or {}
    try:
        fields = line.split()
        min_free, peak_alloc, peak_res, res_start = (float(v) for v in fields[1:5])
        torch_version = fields[5] if len(fields) > 5 else ""
    except ValueError:
        log.warn(f"bad vram line: {line}")
        return
    pool = float(est.get("free_gb", 0.0)) + float(est.get("resident_gb", 0.0))
    used = pool - min_free if pool else float("nan")
    log.info(f"vram: estimated {est.get('estimate_gb', '?')} GB, used {used:.1f} GB (preflight free "
             f"{est.get('free_gb', '?')} + resident {est.get('resident_gb', '?')} - lowest free {min_free:.1f}); "
             f"torch peak allocated {peak_alloc:.1f}, peak reserved {peak_res:.1f}, held at start {res_start:.1f}; "
             f"warned={est.get('warned', False)} state={state}; {est.get('gpu', '?')} {est.get('total_gb', '?')} GB, "
             f"driver {est.get('driver', '?')}, torch {torch_version or '?'}")
    if not est:
        return  # no preflight numbers (smoke tools, dummy): nothing to compare in the CSV
    path = os.path.join(os.path.dirname(log_path()), "vram_stats.csv")
    row = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "job": m.get("job_id"), "state": state,
           "precision": m.get("precision"), "width": m.get("width"), "height": m.get("height"),
           "model_w": est.get("model_w"), "model_h": est.get("model_h"), "model_frames": est.get("model_frames"),
           "chunks": len(m.get("chunks", [])), "resident": int(not m.get("unload", True)),
           "gpu": est.get("gpu"), "total_gb": est.get("total_gb"), "driver": est.get("driver"),
           "torch": torch_version,
           "estimate_gb": est.get("estimate_gb"), "free_gb": est.get("free_gb"),
           "resident_gb": est.get("resident_gb"), "reserve_gb": est.get("reserve_gb"),
           "used_gb": round(used, 2), "lowest_free_gb": min_free, "torch_peak_alloc_gb": peak_alloc,
           "torch_peak_reserved_gb": peak_res, "held_at_start_gb": res_start, "warned": int(bool(est.get("warned")))}
    try:
        new = not os.path.exists(path)
        with open(path, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(row))
            if new:
                w.writeheader()
            w.writerow(row)
    except OSError as exc:
        log.warn(f"could not write {path}: {exc}")


REFRESH_DIR = os.path.expanduser("~/.cache/ltx-alphagen/run/refresh")


def started_by_flame(pid: int) -> bool:
    """True if pid is a live Flame process (not a smoke tool or test, which must not make the
    hook step the frame in a Flame session that happens to be open)."""
    return oslayer.process_name(pid) == "flame"


def signal_refresh(job_id: str, node_id: str, state: str) -> None:
    """Decision 49 (b): a marker for the Flame hook (flame_hook/ltx_alphagen_hook.py), which
    re-renders the node from Flame's UI thread. Markers older than an hour are swept (no hook)."""
    try:
        os.makedirs(REFRESH_DIR, exist_ok=True)
        for name in os.listdir(REFRESH_DIR):
            path = os.path.join(REFRESH_DIR, name)
            if time.time() - os.path.getmtime(path) > 3600:
                os.unlink(path)
        tmp = os.path.join(REFRESH_DIR, f".{job_id}.tmp")
        with open(tmp, "w") as f:
            json.dump({"job_id": job_id, "node_id": node_id, "state": state, "time": time.time()}, f)
        os.replace(tmp, os.path.join(REFRESH_DIR, f"{job_id}.json"))
    except OSError as exc:
        log.warn(f"could not write the refresh marker: {exc}")


EOF_LINE = None  # sentinel put on the queue when the worker side has finished


class SubprocessChannel:
    """Dummy worker: one process per job, lines from its stdout."""

    def __init__(self, cmd: list[str]) -> None:
        log.info("worker command: " + " ".join(cmd))
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, start_new_session=True)
        log.info(f"worker pid {self.proc.pid}")
        self.lines: queue.Queue = queue.Queue()

        def pump():
            for line in self.proc.stdout:
                self.lines.put(line.rstrip("\n"))
            self.lines.put(EOF_LINE)

        threading.Thread(target=pump, daemon=True).start()

    def kill(self) -> None:
        os.killpg(self.proc.pid, signal.SIGKILL)

    def finish(self) -> str:
        status = self.proc.wait()
        return f"worker crashed (signal {-status})" if status < 0 else ""

    def exit_text(self) -> str:
        return f"exit {self.proc.returncode}"


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


class ResidentChannel:
    """Real worker: the resident server. Starts it when needed, then streams one job."""

    def __init__(self, m: dict, request: dict, progress: "Progress", tracker: Tracker) -> None:
        self.sock_path = m["worker_socket"]
        self.info_path = self.sock_path + ".json"
        self.m, self.request, self.progress, self.tracker = m, request, progress, tracker
        self.lines: queue.Queue = queue.Queue()
        self.server_pid = 0
        self.proc = None  # set when this job started the server: it is our child, so poll() reaps it
        for attempt in range(3):
            self._ensure_server()
            conn = self._connect()
            conn.sendall((json.dumps(request) + "\n").encode())
            f = conn.makefile("rb")
            first = f.readline().decode().rstrip("\n")
            if first == "restart":
                log.info(f"resident worker (pid {self.server_pid}) has other settings; replacing it")
                conn.close()
                self._wait_exit(self.server_pid, 60)
                continue
            log.info(f"resident worker pid {self.server_pid} accepted the job (attempt {attempt + 1})")
            self.lines.put(first)

            def pump():
                for raw in f:
                    self.lines.put(raw.decode().rstrip("\n"))
                self.lines.put(EOF_LINE)

            self.conn = conn
            threading.Thread(target=pump, daemon=True).start()
            return
        raise RuntimeError("could not get a resident worker with matching settings")

    def _info(self) -> dict:
        try:
            with open(self.info_path) as f:
                return json.load(f)
        except (OSError, json.JSONDecodeError):
            return {}

    def _ensure_server(self) -> None:
        os.makedirs(os.path.dirname(self.sock_path), exist_ok=True)
        with open(self.sock_path + ".lock", "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)  # one starter at a time (two Process presses)
            info = self._info()
            if info.get("pid") and pid_alive(info["pid"]) and info.get("state") in ("loading", "ready", "busy"):
                self.server_pid = info["pid"]
                log.info(f"reusing resident worker pid {self.server_pid} ({info.get('state')}, "
                         f"{info.get('precision')}, {info.get('vram_gb')} GB reserved)")
                return
            m = self.m
            cmd = [m["worker_python"], "-I", "-B", m["worker"], "--serve", self.sock_path,
                   "--precision", m["precision"], "--flame-pid", str(m.get("flame_pid", 0)),
                   *m.get("worker_args", [])]
            if self.request.get("unload", True):
                cmd.append("--rebuild-transformer")  # decision 55: lower peak when not resident
            out = open(os.path.join(os.path.dirname(self.sock_path), "worker.out"), "w")
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                    start_new_session=True)  # outlives this job when resident
            self.server_pid, self.proc = proc.pid, proc
            log.info(f"started resident worker pid {proc.pid}: " + " ".join(cmd))
            deadline = time.time() + 120
            while time.time() < deadline and self._info().get("pid") != proc.pid:
                if proc.poll() is not None:
                    raise RuntimeError(f"worker exited at start (exit {proc.returncode}); see worker.out")
                time.sleep(0.2)

    def _connect(self) -> socket.socket:
        deadline = time.time() + 900
        while time.time() < deadline:
            if self.progress.cancelled():
                raise KeyboardInterrupt
            info = self._info()
            if info.get("state") == "failed":
                raise RuntimeError(f"worker failed to load: {info.get('message')}")
            if self.server_pid and not self._server_alive():
                raise RuntimeError(f"worker pid {self.server_pid} died while loading; see worker.out")
            if info.get("state") == "busy":
                self.tracker.note("Waiting for the worker (busy with another job)")
            elif info.get("state") == "loading":
                self.tracker.note("Loading the model")
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.sock_path)
                return s
            except (FileNotFoundError, ConnectionRefusedError):
                s.close()
                time.sleep(0.3)
        raise RuntimeError("timed out waiting for the resident worker")

    @staticmethod
    def _wait_exit(pid: int, seconds: float) -> None:
        deadline = time.time() + seconds
        while pid_alive(pid) and time.time() < deadline:
            time.sleep(0.2)

    def kill(self) -> None:
        # Cancel must really free the GPU (Mac test 2026-10-13: GPU looked busy after a Cancel,
        # and the end line said "alive" because it was written 57 ms after the kill). Wait for
        # the worker to be gone and log what actually happened; on the M1 a SIGKILLed process
        # with ~50 s of queued MPS work exits in ~1.5 s.
        t0 = time.time()
        for attempt in (1, 2):
            try:
                os.killpg(self.server_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            while self._server_alive() and time.time() - t0 < 10 * attempt:
                time.sleep(0.1)
            if not self._server_alive():
                log.info(f"cancel: worker pid {self.server_pid} exited {time.time() - t0:.1f} s after SIGKILL")
                return
        log.error(f"cancel: worker pid {self.server_pid} still alive {time.time() - t0:.0f} s after two SIGKILLs")

    def _server_alive(self) -> bool:
        # A killed child stays a zombie (kill(pid, 0) still succeeds) until we reap it.
        if self.proc is not None and self.proc.pid == self.server_pid:
            return self.proc.poll() is None
        return pid_alive(self.server_pid)

    def finish(self) -> str:
        # Only called when the job ended without a result. The socket closes while a killed
        # server is still being torn down, so give it a moment to finish dying before deciding.
        deadline = time.time() + 5
        while self._server_alive() and time.time() < deadline:
            time.sleep(0.1)
        if self._server_alive():
            return ""
        code = self.proc.returncode if self.proc is not None and self.proc.pid == self.server_pid else None
        how = f" (signal {-code})" if code is not None and code < 0 else ""
        return f"the worker process (pid {self.server_pid}) ended unexpectedly{how}; see run/worker.out"

    def exit_text(self) -> str:
        return f"resident pid {self.server_pid} {'alive' if self._server_alive() else 'exited'}"


def main() -> int:
    manifest_path = sys.argv[1]
    ui = "--no-ui" not in sys.argv
    with open(manifest_path) as f:
        m = json.load(f)
    job_id, root = m["job_id"], m["cache_root"]
    log.node, log.job = m.get("node_id", "-"), job_id
    keys, contrib, chunks = m["frame_keys"], m["contrib"], m["chunks"]
    roi, merge = m.get("roi"), m.get("merge", "smooth")
    staging = os.path.dirname(os.path.abspath(manifest_path))  # the plugin's stagingDir()
    out_prefix = os.path.join(staging, f"ltxag_job_{job_id}_out")
    pid_file = os.path.join(staging, f"ltxag_job_{job_id}.pid")
    with open(pid_file, "w") as f:
        f.write(str(os.getpid()))
    cache.mark_pending(root, keys, os.getpid())
    t_start = time.time()
    log.info(f"start: {m['frames']} frames {m['width']}x{m['height']}, chunks={chunks}, "
             f"merge={merge} roi={roi} holds={m.get('holds')} precision={m['precision']} simulate={m['simulate']} sim_seconds={m['seconds']} "
             f"python={sys.executable} {sys.version.split()[0]} ui={ui}")
    cache.write_status(root, job_id, "running", "", chunks_done=0, chunks=len(chunks),
                       frames_done=0, frames=len(keys))

    progress = Progress(m.get("title", "LTX Alpha Gen"), ui)
    holds = m.get("holds") or [1] * len(chunks)
    tracker = Tracker(chunks, len(keys), holds)
    ticking = threading.Event()

    def tick() -> None:
        while not ticking.wait(1.0):
            progress.update(*tracker.render())

    threading.Thread(target=tick, daemon=True).start()
    state, message = "failed", "worker exited without a result"
    vram_line = ""
    channel = None
    try:
        if m.get("worker_python"):
            request = {"job_id": job_id, "node_id": m.get("node_id", "-"), "in": m["input"],
                       "chunks": chunks, "out_prefix": out_prefix, "colour": m.get("colour", "Bypass"),
                       "colour_tag": m.get("colour_tag", ""), "ocio_config": m.get("ocio_config", ""), "precision": m["precision"],
                       "model_dir": m["model_dir"], "unload": bool(m.get("unload", True)),
                       "seed": int(m.get("seed", 1234)), "holds": holds}
            channel = ResidentChannel(m, request, progress, tracker)
        else:
            channel = SubprocessChannel(
                [sys.executable, "-I", "-B", m["worker"], "--in", m["input"],
                 "--chunks", ",".join(f"{s}:{n}" for s, n in chunks), "--out-prefix", out_prefix,
                 "--seconds", str(m["seconds"]), "--simulate", m["simulate"], "--precision", m["precision"]])
    except KeyboardInterrupt:
        state, message = "cancelled", "cancelled by user"
    except Exception as exc:  # noqa: BLE001 - becomes the job's failure message
        log.error(f"could not start the worker: {exc}")
        message = str(exc)
    t_chunk = time.time()

    results: dict[int, np.ndarray] = {}
    finished: set[int] = set()

    def finalize(f: int) -> None:
        parts = contrib[f]
        if f in finished or any(c not in results for c, _ in parts):
            return
        if merge == "flow" and len(parts) == 2:
            (ca, _), (cb, wb) = parts
            matte = morph.morph(results[ca][f - chunks[ca][0]].astype(np.float32),
                                results[cb][f - chunks[cb][0]].astype(np.float32), wb)
        else:
            matte = sum(w * results[c][f - chunks[c][0]].astype(np.float32) for c, w in parts)
        if roi:
            full = np.zeros((m["full_height"], m["full_width"]), np.float32)
            full[roi[1]:roi[1] + roi[3], roi[0]:roi[0] + roi[2]] = matte
            matte = full
        cache.write_matte(root, keys[f], matte)
        cache.clear_pending(root, [keys[f]])
        finished.add(f)

    while channel is not None:
        if progress.cancelled():
            log.info("cancel pressed in the progress window: killing worker")
            channel.kill()
            state, message = "cancelled", "cancelled by user"
            break
        try:
            line = channel.lines.get(timeout=0.2)
        except queue.Empty:
            continue
        if line is EOF_LINE:
            break
        known = line.split(" ", 1)[0] in ("progress", "info", "chunk_done", "done", "error", "stage", "step", "vram")
        if line.startswith(("progress ", "stage ", "step ")):
            log.debug(f"worker: {line}")
        elif line.startswith("error "):
            log.error(f"worker: {line}")
        elif known:
            log.info(f"worker: {line}")
        else:
            log.warn(f"worker output: {line}")  # stderr, e.g. a Python traceback
        if line.startswith("progress "):
            tracker.note((line.split(" ", 2) + [""])[2])
        elif line.startswith("stage "):
            _, i, name = line.split(" ", 2)
            tracker.on_stage(int(i), name)
        elif line.startswith("step "):
            tracker.on_step(*(int(v) for v in line.split()[1:4]))
        elif line.startswith("vram "):
            vram_line = line
        elif line.startswith("chunk_done "):
            i = int(line.split()[1])
            tracker.on_chunk_done(i)
            s, n = chunks[i]
            results[i] = np.fromfile(f"{out_prefix}_{i}.bin", dtype="<f2",
                                     offset=transport.HEADER.size).reshape(n, m["height"], m["width"])
            os.unlink(f"{out_prefix}_{i}.bin")
            for f in range(s, s + n):
                finalize(f)
            for j in list(results):
                js, jn = chunks[j]
                if all(f in finished for f in range(js, js + jn)):
                    del results[j]
            log.info(f"chunk {i + 1}/{len(chunks)} done in {time.time() - t_chunk:.1f} s; "
                     f"{len(finished)}/{len(keys)} frames cached")
            t_chunk = time.time()
            cache.write_status(root, job_id, "running", "", chunks_done=i + 1, chunks=len(chunks),
                               frames_done=len(finished), frames=len(keys))
        elif line == "done":
            state, message = "done", ""
        elif line.startswith("error "):
            state, message = "failed", line[6:]

    if state == "done" and len(finished) != len(keys):
        state, message = "failed", f"only {len(finished)} of {len(keys)} frames were produced"
    if state == "failed" and message == "worker exited without a result" and channel is not None:
        crash = channel.finish()  # never hides a specific error the worker reported (e.g. oom)
        if crash:
            message = crash
    ticking.set()
    progress.close()
    if vram_line:
        record_vram(m, vram_line, state if not message.startswith("oom") else "oom")
    cache.clear_pending(root, keys)
    cache.write_status(root, job_id, state, message, chunks_done=len(chunks) if state == "done" else -1,
                       chunks=len(chunks), frames_done=len(finished), frames=len(keys))
    for p in [m["input"], manifest_path, pid_file] + [f"{out_prefix}_{i}.bin" for i in range(len(chunks))]:
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    if started_by_flame(m.get("flame_pid", 0)):
        signal_refresh(job_id, m.get("node_id", "-"), state)
    summary = (f"end: {state} {message} ({len(finished)}/{len(keys)} frames cached, "
               f"{channel.exit_text() if channel else 'no worker'}, {time.time() - t_start:.1f} s)")
    (log.info if state == "done" else log.warn if state == "cancelled" else log.error)(summary)
    if state == "failed":
        Progress.error("LTX Alpha Gen", f"Processing failed: {message}", ui)
    return 0 if state == "done" else 1


if __name__ == "__main__":
    sys.exit(main())
