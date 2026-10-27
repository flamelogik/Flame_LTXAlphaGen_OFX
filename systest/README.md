# System test

An end-to-end test of the plugin bundle, run before every release package. A small OFX host
(`host/`) loads the bundle the way Flame does and drives it through the same actions a user
triggers: create a node, press Process, wait for the job, render frames, press Cancel. The real
job controller (`job.py`) and worker run underneath. Results are compared with what is known
about the synthetic test clip. Interactive testing in Flame is still needed: the layout, the
Flame Python hook and anything Flame does differently from this host are not covered.

## Tiers

| Tier | Bundle | Worker | Time | What it covers |
|---|---|---|---|---|
| `fast` | dev build (`LTXAG_DEV_BUILD=ON`) | dummy (luminance) | ~1 min | load/describe, defaults, licence gate, markers, Process with 2 chunks, Smooth and Flow Warp merge, cache key, purge, ROI crop / Multiply / Black Out, empty alpha, Cancel, simulated OOM and crash, the memory Yes/No warning, Save Default, fp8 refused on Mac, Collect Logs |
| `full` | release build (`OFF`) | the real model | a few min (Linux) / ~15 min (M1) | the gate as shipped (Prototype params hidden), a real 512×512×33 matte checked against the clip's truth, VRAM record, Unload GPU leaves no worker, Cancel during denoising |

The fast tier needs a dev build because release builds compile the dummy worker out. The full
tier tests the exact release binary.

## How the host behaves like Flame

As recorded in ofx/CLAUDE.md ("Flame host behaviour"):

- every action runs on the calling thread;
- every plugin `setValue` comes straight back as `instanceChanged` (reason "plugin edited").
  Nesting deeper than 32 is reported as an error (the recursion that crashed Flame);
- param values set from another thread are applied only at the next user action or render;
- the output stays RGBA whatever `getClipPreferences` asks for;
- frames are 32-bit float RGBA;
- `sendMessage` questions get a scripted Yes, No or no answer;
- no colour management: the Source clip has no colourspace tag unless a test sets one, and the
  clip preferences answer (the Input Colour request) is only read by tests that ask for it;
- a test can create a node "from a saved setup" (`Host::savedValues`, e.g. the old Input Colour).

The host also reports any action or message that arrives from a non-main thread.

## Isolation

`run_systest.sh` gives each run a fresh HOME under `--work`: config, matte cache, logs and the
resident worker's socket all live there, so a Flame session on the same machine is not touched.
From the real user's setup it reads `ltx_python` and `model_dir`, and it links the prompt
embedding (`prompt-empty.safetensors`) from the real cache. Nothing is written to the real setup.

Instead of zenity or the Swift progress window, job.py gets `fake_ui.py`. It is first on PATH on
Linux. On macOS it is pre-placed as the compiled helper in the test HOME. It logs the progress
lines and "presses Cancel" when the test creates `<ui>/cancel`, so Cancel runs job.py's real
cancel path. On macOS, job.py's failure dialog (osascript, absolute path) can't be faked: the
test closes the ones it caused.

## When

- **Fast tier:** after every completed feature or fix, on both platforms
  (`TIERS=fast scripts/system_test.sh`).
- **Both tiers:** for every release, on the clean commit being released, before
  `package_release*.sh` (they check for the recorded `PASS`).

## Running

All machines in parallel, from the development checkout:

```
LINUX_HOST=<rocky host> LINUX_WORK=<local-disk folder there> \
MAC_HOST=<mac> MAC_REMOTE=<build folder> MAC_WORK=<local-disk folder> \
MAC_CMAKE=<cmake on the Mac> MAC_CMAKE_ARGS="<as for build_mac.sh>" \
scripts/system_test.sh
```

Results go to `dist/systest/<commit>/<linux|macos>/` (`out-<tier>/junit-<tier>.xml`,
`console.log`, `logs.tar.gz` with the plugin/job/worker logs). A platform that passed both tiers
on a clean checkout gets a `PASS` file. `scripts/package_release.sh` and
`package_release_mac.sh` refuse to package without one (`SKIP_SYSTEST=1` overrides). Options:
`TIERS=fast` (quick check), `ONLY=roi,cancel` (single tests; never records a PASS).

On one machine by hand: `systest/run_all.sh --build-root <dir> --work <dir>`, or one tier
against an existing build with `systest/run_systest.sh --tier fast --bundle … --host … --work …`.
The full tier needs a completed Setup for the user running it.

## Limits

- The dummy matte is the same in every chunk, so the fast tier checks that merging works but not
  the blend weights (unit tests cover those: `chunk_plan`, `test_job`).
- Not covered: Download/Setup (`tools/setup_smoke.sh`), View Licence, the Flame hook's
  auto-refresh (job.py only writes refresh markers for a process named `flame`), the layout,
  whether Flame honours the Input Colour request, OCIO colour conversion with the real model, 6K, resident-worker reuse between jobs
  (`tools/resident_smoke.py`).

## Why it is built this way (decision 67, user choices 2026-10-13)

- **Our own Flame-like host**, not the OpenFX HostSupport library: HostSupport behaves like a
  generic host, and the point is Flame's quirks (echoed setValues, lazy off-thread changes).
- **Fast tier on a dev build:** release builds compile the dummy worker out, and a test switch in
  shipping code was rejected. The full tier tests the exact release binary.
- **Fake windows from the harness** instead of a product switch, so Cancel runs job.py's real path.
- **Checked by mutation:** an unconditional label write in `changedParam` fails with the recursion
  error; a 50% overlay fails every frame.
