# CLAUDE.md — LTX Alpha Gen OFX for Flame

Design knowledge for anyone changing this plugin, human or AI assistant: what was decided and
why, how Flame behaves as an OFX host, the measurements behind the memory model, and what was
tried and failed. Numbered decisions are referenced from code comments ("decision 34"); keep the
numbers stable. **This file is the overview and the decision index; the detail lives next to the
code** (decision 69):

| File | Covers |
|---|---|
| `ofx/CLAUDE.md` | the C++ plugin: params and UI, gate, render, markers, cache, ROI, chunking, preflight and the memory model, Flame host behaviour, the C++ platform layer (Linux, macOS) |
| `worker/CLAUDE.md` | job.py and the workers: manifest, line protocol, resident server, colour, merge, the MPS worker (macOS), the Python OS layer |
| `helper/CLAUDE.md` | Setup: runtime install, models, licence, lockfile, macOS Setup |
| `flame_hook/CLAUDE.md` | the Flame Python hook (auto-refresh) and Flame's Python API |
| `systest/README.md` | the system test (decision 67) |

Claude Code loads a folder's `CLAUDE.md` when it touches a file in that folder. All design notes
are plain files in the module folders, never in hidden folders (user: transparency).
Machine-specific notes (hosts, paths, accounts, status) go in the git-ignored `CLAUDE.local.md`.

Tags: **DECIDED** (maintainer approved), **PROPOSED** (recommended, not yet approved), **OPEN**
(needs a decision), **UNVERIFIED** (not yet confirmed against a real install or source).

## Goal

An OFX plugin for Autodesk Flame on **Rocky Linux 9** (NVIDIA) and, since 0.1.4, **macOS on Apple
Silicon** (PyTorch-MPS, decision 65) that runs Lightricks' **LTX-2.5 Alpha Gen** IC-LoRA and
produces an alpha matte from an RGB clip. It can be shared publicly: **users download their own
weights and are responsible for their own LTX licence.** v0.1 (2026-10-11) is the first release;
see README.md for the user-facing view.

## Architecture

```
Flame ──OFX──> LTXAlphaGen.ofx (C++ shim, ofx/)
                 render():  hash source frame t → disk cache lookup → matte | marker (+ overlay)
                 Process:   preflight (memory) → fetch all clip frames (main thread, ~11 ms/HD frame)
                            → staging folder: ltxag_job_<id>_in.bin + manifest .json, pending markers
                            → launchDetached(python3 -I -B job.py manifest)
job.py (worker/): progress window + Cancel, status file, refresh marker
   └─> resident worker server (LTX venv python -I -B ltx_worker.py --serve)  | dummy_worker.py
         reads chunks, converts colour, pads, infers, crops → out_<i>.bin; stage/step/vram lines
   job.py blends overlaps → writes one .matte per frame into the cache → jobs/<id>.json
Flame Python hook (flame_hook/): polls refresh markers → steps the current frame and back
Setup (helper/setup_helper.py): runtime, models, prompt cache → user config
```

Other folders: `packaging/install.sh` (in the release tarball), `scripts/` (build, package,
system test), `tools/` (`worker_smoke.py`, `job_real_smoke.py`, `resident_smoke.py`,
`setup_smoke.sh`, `make_test_clip.py`, `measure_run.sh` (CLI VRAM/time runs), `nvml_probe.cpp`,
`mac_bench.py`, `hold_ab.py` (Frame Hold A/B, 74)), `mlx/PORT_MAP.md` (the
planned MLX port), `third_party/openfx` (OpenFX `OFX_Release_1_4_TAG`, `include/` + `Support/`
only, BSD, `VENDORED.txt`). Plugin id `com.janklier.ltx.alphagen`, version from CMake
`LTXAG_VERSION` (OFX version = its major.minor), grouping "LTX".

## Decision index

**Cross-cutting (detail here)**
- **1.** A light C++ OFX shim plus a Python background process on the same machine. **No ComfyUI.**
- **5.** Progress, Cancel, graceful OOM; the plugin must never kill Flame.
- **6.** All GPU memory goes back to Flame when processing ends (Unload GPU on, the default).
- **15/16.** Recipe 1 only: model-card recipe, distilled transformer, IC-LoRA, stage 1 at native
  resolution, empty prompt.
- **17.** Only the bf16 transformer is downloaded; runtime precision per node: **bf16 or
  fp8-cast** (fp16 on some Macs, 68). nvfp4 and int8 dropped.
- **22.** Flame's shipped NVIDIA driver runs the stack (verified: 580.159.04, torch 2.13 + CUDA 13.2).
- **35.** Processing never starts automatically: render serves only from the cache; **Process**
  stages the frames and launches a detached job with its own progress window and Cancel.
- **39.** Diagnostic logging is a product requirement (see "Logging").
- **50.** No more prototype rounds; developed and tested as the real thing.
- **53.** **Project licence: MPL 2.0** (`LICENSE`, from `mozilla.org/media/MPL/2.0/index.txt`). File-level copyleft; vendored
  OpenFX keeps BSD; LTX code and weights are never in the repo.
- **64.** OS-specific code sits behind one small layer per language: C++ `ofx/src/platform.h`,
  Python `worker/ltxag_worker/oslayer.py`. The inference backend (torch vs MLX) is a separate
  axis: the worker line protocol and the manifest are its boundary, so an MLX worker speaks the
  same protocol (a `backend` manifest field and cache-key part would come with it).
- **65.** macOS runs on **PyTorch-MPS**. A from-scratch MLX port was to follow only if it clearly
  beat MPS; **dropped** (user, 2026-10-14: the M5 benchmark shows no gain, `worker/CLAUDE.md`);
  `mlx/PORT_MAP.md` stays as a model reference. Detail: `worker/CLAUDE.md`, `ofx/CLAUDE.md`.
- **67.** **System test**: a Flame-like OFX host of our own; fast tier (dev build, dummy) after
  every feature; the full tier (release build, real model) is run by a maintainer on real hardware
  against the CI-built release (77), no longer as a gate in packaging (`systest/README.md`).
- **77. PROPOSED (agent-built 2026-10-27, not yet run on GitHub): release process.** Pushing a tag
  `v<LTXAG_VERSION>` runs `.github/workflows/release.yml`: Rocky 9 container (Linux) + `macos-15`
  arm64, release build, ctest, fast system tier on a dev build (dummy worker, no GPU), package
  (`scripts/package_release.sh`, local by default, `HOST=` for ssh) + `.sha256`; only if both pass, a
  GitHub **pre-release** is published. A maintainer runs `scripts/maintainer_test.sh <tag>` (full tier
  on the packaged bundle) on a GPU Linux box and a Mac and uploads `ltxag-test-<platform>-<tag>.json`
  (tarball sha256, no host or user names); `promote.yml` (`scripts/promote.sh`) clears the pre-release
  flag only with a matching PASS for both. Mac CI builds pin the deployment target to 15.0.
  `scripts/system_test.sh` stays as an optional multi-machine wrapper. `CONTRIBUTING.md` documents it.
- **69.** CLAUDE.md split per module (user: keep each under ~250 lines, 2026-10-14; not a to-do
  below that); no hidden
  documentation files (no `.claude/rules/`).
- **76. Input Colour = Auto / Bypass (user, 2026-10-27; replaces the dropdown; 27/54 still hold).**
  Auto asks Flame for `ACEScg` (`OfxImageClipPropPreferredColourspaces_Source`, GetClipPreferences out args);
  the worker keeps the fixed ACES 2.0 SDR `DisplayViewTransform`, so the model input is unchanged. Flame's
  display colourspaces are encoding-only (no tone mapping), so requesting Rec.1886 was rejected. **Image
  output:** Auto = the picture converted ACEScg -> display in the plugin (fixed matrix + gamma 2.4, checked
  against OCIO), then the overlay, tagged `Rec.1886 Rec.709 - Display`; Bypass = the matte on both outputs,
  tagged `Matte` (the clip's colourspace is unknown to us, and OCIO stays out of the shim, 54). VERIFIED in
  Flame 2027.2 on several clips (user, 2026-10-27). Details: `ofx/CLAUDE.md`, `worker/CLAUDE.md`,
  `docs/colour-auto.md`.
- **73. PROPOSED (user, 2026-10-15): cloud GPU backend**, for community users without a big
  enough GPU who may send footage online. A Setup choice (Local / Cloud), not a sister plugin:
  colour, chunks, merge, cache, markers and progress stay local; only `job.py`'s worker channel
  changes, and our own `ltx_worker.py` runs remotely (same recipe; no hosted Alpha Gen API
  exists: LTX API, fal, HF providers checked 2026-10-15). User needs little more than an API key;
  spin up, process, spin down, optional keep-warm (like Unload GPU). **RunPod Serverless first**
  (RTX PRO 6000 96 GB, ~$3.49/hr listed; frames via its S3-compatible API on a RunPod network
  volume, which needs a second RunPod S3 key; the volume pins the endpoint's datacenter), **AWS
  EC2 + our AMI as the backup** (isolation in the user's own account; GPU vCPU quota starts at 0
  and must be requested per region). Weights never in the image: Setup downloads them with the
  user's HF token into the user's own cloud storage. OPEN: cold start, live availability, the
  serverless progress path; a paid spike on a RunPod account first.

**In `ofx/CLAUDE.md`:** 11/19 Unload GPU param · 12/32 VRAM reserve · 18/21 three-level defaults ·
26/72 resolution limit (6K refused) · 30/61 chunks and Chunk Size · 34 no silent downgrade · 36 disk cache ·
37/52 layout · 38 licence + model gate · 41 caption labels · 44 markers · 46 paths logged as-is ·
48 RGBA output · 56 memory warning and record · 58 matte overlay · 59 SETUP REQUIRED on both
outputs · 62 Merge (Smooth in C++) · 63 ROI = source alpha · 64/65 C++ platform layer and macOS
memory · 68 effective precision (fp16) · 71 processed record: pass-through or failed render ·
74 Frame Hold param, held chunks · 75 Auto hold (motion while staging, per-chunk plan).

**In `worker/CLAUDE.md`:** 8/23/27/28/54 input colour (OCIO in the worker) · 9 prompt cache ·
24 padding · 33 idle timeout · 55 resident transformer only with Unload off · 56 VRAM record
(worker side) · 62 Flow Warp · 64 Python OS layer · 65 MPS worker · 68 fp16 worker · 72 no 6K (won't do) ·
74 Frame Hold (why: 8 frames per latent; the A/B).

**In `helper/CLAUDE.md`:** 2/13/20 Setup downloads, token used once · 7 LTX packages installed by
Setup · 51 reuse existing downloads · 57 no bundled Python packages · 60 uv-managed Python 3.14.7
· 66 link instead of copy · 68 fp16 probe and copy (macOS).

**In `flame_hook/CLAUDE.md`:** 49 auto-refresh via a Flame Python hook · 70 mattes as shelf media (shelved).

## Model and runtime facts

In `worker/CLAUDE.md`: the model (Alpha Gen IC-LoRA on LTX-2.5-22B), its limits, the required
files (~73 GB), the pinned stack (LTX-2 `9ec55f9`, torch 2.13.0+cu132 / 2.14.1 on macOS) and the
LTX licence terms (paid licence at ≥ USD 10M revenue; no separate legal read: closed by the user, 2026-10-14).

## Logging (decision 39)

- One log, `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`, for plugin, job, worker, Setup and hook,
  rotated at plugin load (10 MB × 5). Line format `YYYY-MM-DD HH:MM:SS.mmm LEVEL comp[pid/tid]
  node=<id> job=<id> | msg`; C++ (`ofx/src/log.cpp`) and Python (`worker/ltxag_worker/log.py`) must
  stay identical (`test_log.py`).
- Plugin load: environment snapshot (version + git, host, OS, glibc, CPU, RAM, staging space,
  GPU/driver/VRAM, config without secrets) and sweep results. INFO: every non-render action;
  DEBUG: renders and cache hits. Per job: params, chunk plan, timings, worker output (tracebacks at
  WARN), exit classification, VRAM record (also `logs/vram_stats.csv`).
- Never logged: the HF token (length only), image data, clip or project names.
- **Collect Logs** → `~/ltx-alphagen-support-<ts>.tar.gz` (`logs/` + `environment.txt` + `jobs/`).

## Building, testing, releasing

- **Linux: build on Rocky 9** (glibc ≤ 2.34, GCC 11): newer distributions make binaries Flame
  can't load. `HOST=<ssh host> scripts/build_remote.sh` (sync, release build, ctest);
  `scripts/package_release.sh` makes the tarball in `dist/` (git-ignored; local, or `HOST=` over ssh;
  CI uses the local mode, 77). Local test builds on
  any Linux: `cmake -S . -B <build> -DWORKER_TEST_PYTHON=<python with numpy importable under -I>`.
- **Build facts:** **C++14** (OFX 1.4 Support uses dynamic exception specs and `auto_ptr`);
  **libstdc++ linked dynamically** (Flame uses the system GCC 11 one; `libstdc++-static` isn't on
  Rocky); the `.ofx` exports only the two OFX entry points; `setEvaluateOnChange` lives on
  `ValueParamDescriptor`; `posix_spawn` needs absolute paths;
  workers run as `python -I -B` (dependencies in the interpreter's own site-packages);
  `-DLTXAG_GIT=<hash>` names the build when building from a copy without `.git`.
  `LTXAG_DEV_BUILD=ON` (the CMake default) adds the Prototype page (Sim Seconds, Simulate, Sim VRAM
  GB, Worker Python, Enforce Gate, Auto Refresh, Use Dummy); release builds (`OFF`) hide those
  params but still define them, so saved setups load.
- **macOS** (Apple Silicon, Command Line Tools: clang 17, MacOSX 26.2 SDK, cmake 4.4):
  `scripts/build_mac.sh`, `scripts/package_release_mac.sh` (`HOST`, `REMOTE`, `CMAKE`,
  `CMAKE_ARGS`; `LTXAlphaGen-<v>-macos-arm64.tar.gz`). Bundle binary in
  `Contents/MacOS/LTXAlphaGen.ofx` (arm64, minos 15.0 only if built with `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0`: the published v0.2.1 has minos 26.0) with a generated minimal `Info.plist`
  (whether Flame needs it: UNVERIFIED); exports via `-Wl,-exported_symbols_list` and the vendored
  `osxSymbols`; ld's default `-undefined error` replaces `--no-undefined`;
  links Metal + Foundation, libc++, libobjc, libSystem; signed ad hoc only (no notarisation). A CLT
  install with a stale `usr/include/c++/v1` hides the SDK's libc++ headers (`'string' file not
  found`): pass `-stdlib++-isystem <SDK>/usr/include/c++/v1` in `CMAKE_CXX_FLAGS` and
  `CMAKE_OBJCXX_FLAGS`. A CLT update that leaves an SDK newer than its `ld` (2026-10-17: CLT 26.6,
  `xcrun` picked a leftover MacOSX27.0.sdk; "unknown architecture" in its `.tbd` files): pass
  `-DCMAKE_OSX_SYSROOT=<CLT>/SDKs/MacOSX.sdk`.
- **Tests (ctest, 9, host-free):** `chunk_plan`, `roi`, `motion`, `worker_python` (padding, transport, job
  incl. Tracker, VRAM CSV, refresh marker, ROI paste-back, flow merge, `test_morph`, the hook
  against a fake `flame` module, log, Setup models), `worker_roundtrip`, `cache`, `marker_log`,
  `vram_estimate`, `load_bundle`. Real model on a GPU machine: `tools/worker_smoke.py`,
  `job_real_smoke.py`, `resident_smoke.py`, `setup_smoke.sh`.
- **System test (decision 67):** `scripts/system_test.sh` (both platforms in parallel), or
  `systest/run_all.sh` / `run_systest.sh` on one machine. The Flame-like host catches re-entrancy
  bugs like the setValue recursion; what it does not imitate is still found by testing in Flame.
- **Installer:** `packaging/install.sh` puts the bundle in `/usr/OFX/Plugins` (macOS:
  `/Library/OFX/Plugins`, removing the quarantine flag) and the hook in
  `/opt/Autodesk/shared/python`; refuses to replace a hook file that isn't ours; `--uninstall`;
  replaces a symlinked dev install with a copy (says so); "NOT INSTALLED" while Flame runs; prints
  the version the package scripts stamp into it (`VERSION=dev` unpackaged).

## Roadmap (after v0.1)

- Large jobs: disk staging when `/dev/shm` is too small; skip already-cached chunks.
- **48 GB cards:** answered by a user's RTX A6000 (Ampere, 2026-10-14, `ofx/CLAUDE.md`): fp8
  HD × 73 used 29.4–29.9 GB, bf16 Quarter 41.5 GB (3.2 GB left); fp8 Full 145 (est. 42.2 of 42.1
  available) not tried. fp8-cast runs before Ada too: below sm_89 it skips the Triton rounding
  kernel and upcasts with `weight.to(dtype)` (`ltx_core/quantization/fp8_cast.py:62-95`). Ada
  speed UNVERIFIED.
- Not planned until needed (user, 2026-10-14): decision 56 step 2, an offline install of the
  Python packages (57), an A/B of recipe 2 (`alpha_gen.py`: dev transformer, 30 steps), the
  Background Reactor check. Won't do: the MLX port (65), 6K frames (72).

## Dead ends (cross-cutting; module ones are in the module files)

- Running the model inside `render()`: it freezes Flame's UI thread.
- Off-the-shelf static libstdc++: not installed on Rocky, and unnecessary.
- nvfp4 (Blackwell-only, needs a compile step) and the int8 `comfy-int8-convrot` checkpoint (no
  loader outside ComfyUI).
- Watching for a process with `pgrep -f '<text>'` over ssh: it matches its own command line. Use
  marker files.
- Staging files named `ltxag_<word>_…` from tools: any plugin load (Flame, the system test)
  sweeps them as stale (owner pid parsed as 0, `cache.cpp:225`) and the running tool loses its
  input. Name them `ltxag_<pid>_…` (`hold_ab.py`); `worker_smoke.py` still uses `ltxag_smoke_<pid>`.
  Don't run the system test next to a smoke or A/B run anyway (user, 2026-10-17).

## Conventions

- Verify every API, flag or config key against installed source or docs before using it, and
  cite where (file and line, or the command). Mark anything unconfirmed UNVERIFIED.
- Ask the maintainer rather than assume when a request has two readings.
- Testing has two layers: the unit tests, and a demonstration of each requirement (in Flame for
  anything host-related). Passing unit tests alone is not evidence.
- **Fast system test after every feature** (decision 67): before reporting a feature or fix as
  done, run `TIERS=fast scripts/system_test.sh` on both platforms (invocation in
  `CLAUDE.local.md`) and include its results. A failure means it is not done; if a machine can't
  be reached, say which tier and platform was not run and why.
- **Releases (decisions 67/77):** CI builds, smoke-tests (ctest + fast tier) and publishes a pre-release
  from a `v*` tag; a maintainer runs the full tier (`scripts/maintainer_test.sh <tag>`) on both
  platforms and promotes. Check the Mac for other GPU jobs before the full tier.
- Keep these files current: record decisions with their numbers and reasons in the module file
  (and a line in the index above), move items between PROPOSED/OPEN/DECIDED, add dead ends. Keep
  each file under ~250 lines. Nothing machine- or person-specific: that goes in `CLAUDE.local.md`.
