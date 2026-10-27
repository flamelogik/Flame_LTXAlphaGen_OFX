# Flame_LTXAlphaGen_OFX

OFX plugin for Autodesk Flame to run LTX 2.5 Alpha Gen models

Generate an alpha matte from an RGB clip, directly in Flame, using Lightricks'
**LTX-2.5 Alpha Gen** IC-LoRA. It runs locally on your own GPU: NVIDIA on Linux, or Apple
Silicon on macOS. There is no cloud service and no ComfyUI.

> **Status: v0.2.1.** The plugin works in Flame and has produced good mattes on real footage.
> Setup is guided, and the viewer refreshes by itself when a job finishes. Since 0.1.4: **macOS
> on Apple Silicon** (much slower than a large NVIDIA GPU; see [macOS](#macos-apple-silicon));
> since 0.1.5: about 1.4× faster processing on M1-family Macs (an fp16 copy of the model); new in
> 0.2: a processed node whose matte goes missing shows MATTE MISSING instead of passing through;
> new in 0.2.1: **Frame Hold** for clips with fast motion (sharper, steadier edges; `Auto` holds
> only the parts that need it, see [Motion](#motion)).
> See [Status and roadmap](#status-and-roadmap) for what is not built yet.

## How long does it take?

Estimated time for one HD clip of 73 frames (1920×1080), from pressing Process to the finished
matte:

| Machine | Precision | Encode | Steps (8) | Decode | Total |
|---|---|---|---|---|---|
| NVIDIA RTX PRO 6000 Blackwell (Linux) | fp8 or bf16 | ~2 s | ~71 s | ~12 s | ~1.5–2 min |
| NVIDIA RTX A6000 48 GB (Linux) | fp8 | — | — | — | ~4.7–5 min |
| Mac Studio M5 Ultra, 80-core GPU | bf16 | ~60–190 s | ~160–240 s | ~30–100 s | ~6–10 min |
| Mac Studio M3 Ultra, 80-core GPU | bf16 | ~300 s | ~1120 s | ~165 s | ~28 min |
| Mac Studio M2 Ultra, 76-core GPU | bf16 | ~315 s | ~1170 s | ~170 s | ~30 min |
| Mac Studio M1 Ultra, 64-core GPU | fp16 | 406 s | ~1510 s | ~220 s | ~38 min |

These are estimates. The Blackwell, RTX A6000 and M1 Ultra figures are based on measured runs.
The M2, M3 and M5 figures are worked out from GPU benchmarks and Apple's specifications, and nobody
has run the plugin on those Macs yet. Macs need at least 128 GB of memory for this clip.

## Quick Start

(read full details further down)

- Download the release bundle, unpack it, and run install.sh with 'sudo'. You can find
the install bundles for Linux and MacOS in the right-hand toolbar of GitHub when you
click on releases.
- Launch Flame, setup a new batch group and add 'LTX Alpha Gen' OFX node after a media file
- The first run requires some setup. Open the plugin's Setup tab:

1. View the license file. You are responsible for meeting all LTX license requirements, and
respecting the revenue threshold (USD 10M) above which you need a paid licence from Lightricks.
2. If satisfied, tick 'I Accept' to unlock the rest of the functions.
3. You need to enter your Hugging Face token to facilitate the download. If you don't have
one yet, sign up for a free account to generate one, and accept the terms of the two LTX
model repos there (see Setup below). **Important**: you need to accept the licensing terms 
on Hugging Face for the LTX 2.5 base model and the IC Lora Alpha Gen, otherwise the download
may fail. Please check https://huggingface.co/Lightricks/LTX-2.5 and 
https://huggingface.co/Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen.
4. You can change the folder where models will be stored from the default.
5. Important: if you have previously downloaded these model files, you can provide the 
folder where they are ('Existing Models'), and the plugin will copy them locally instead of
re-downloading them.
6. Click 'Download'. This will download ~80GB and can take 30+ minutes depending on your 
Internet connection. You will see a progress bar during this time.
7. Switch to the main tab of the plugin for the rest.
8. If your media is not in Rec709, set the color space your media is in. The plugin will
convert it to fit the model's requirements.
9. Check the precision of the model. The plugin will automatically check your GPU and 
available VRAM and guide you. If you're low on VRAM, it may be useful to go to the Flame
Resource Manager and purge unused VRAM so the plugin can use it.
10. If you process multiple media, uncheck 'Unload GPU' and the model will be kept in
memory reducing runtime considerably. This needs more VRAM: on a 48 GB card, leave it on.
11. If your GPU has low VRAM, use a smaller chunk size. It reduces the needed VRAM size. 
Be aware the model could change the 'dominant object' in different chunks, and that 
there will be seams. Make an informed choice.
12. You can provide an ROI to focus on one part of the shot. Create a matte outside of
the plugin via standard Flame nodes, and connect it to the matte channel of the plugin.
13. Click the 'Process' button to run the model. It will show a progress bar and time
estimate. Once the processing is complete it will automatically update the viewer. 
In Auto the image output shows the picture (display-referred) with a magenta overlay of the matte, and the matte output has the matte. In Bypass both outputs show the matte.
14. The output is cached locally to avoid re-running the model. If you're done you 
may want to use the 'Purge Cache' button to purge those files. 

The Alpha Gen tab:

![The Alpha Gen tab in Flame](docs/LTX_OFX_page1.png)

The Setup tab:

![The Setup tab in Flame](docs/LTX_OFX_page2.png)


## Origins

Originally written by Jan Klier and contributed to the Logik community of Autodesk Flame

Inspired by recent conversations in the community (including IBC 2026 in Amsterdam), to build
tools collectively and with best practices of software engineering and version management.

Everyone in the community is encouraged to enhance this plugin further or fix issues, 
following the guidelines at https://github.com/flamelogik

## Requirements

- Autodesk Flame on **Linux** (developed and tested on Rocky Linux 9.7 with Flame 2027.1), or on
  **macOS** with Apple Silicon (see [macOS](#macos-apple-silicon) for its own requirements).
- An **NVIDIA GPU with a lot of VRAM**. Measured peak usage with the official LTX pipeline:

  | Size | Frames | bf16 | fp8 |
  |---|---|---|---|
  | 1920×1088 | 49 | 44.9 GB | 27.5 GB |
  | 1920×1088 | 97 | 51.7 GB | 34.1 GB |
  | 1920×1088 | 145 | 58.8 GB | 41.3 GB |
  | 3840×2176 | 49 | 67.8 GB | 54.1 GB |

  fp8 saves about 17 GB with no speed cost. Expect roughly 1.6 s per HD frame. Keeping the model
  loaded between jobs (Unload GPU off) needs more: about 57 GB for HD at 145 frames in fp8.
  6K is not supported: scale the clip down before the node and the matte up after it, or use a
  region of interest (the source alpha) so the processed area fits. A 24 GB card is too small; on a 48 GB card use fp8.
- About **80 GB of disk** in the Model Folder (73 GB of model files plus the ~7 GB LTX runtime),
  plus space for the matte cache (about 4 MB per HD frame).
- Internet access during Setup (Hugging Face, GitHub, PyPI). After Setup, processing runs offline.
- Flame's own NVIDIA driver is sufficient (verified with driver 580.159.04, torch 2.13, CUDA 13.2).
- A **C compiler** (`gcc`): the GPU code compiles small helpers while it runs. Most Flame systems
  have it; if not, `sudo dnf install gcc`. Setup checks for it before downloading anything. No
  Python development packages are needed: Setup installs its own Python (3.14) with everything it
  needs, independent of the system Python.
- A free [Hugging Face](https://huggingface.co) account, if you need to download the model files.

### macOS (Apple Silicon)

The Mac version runs the same model with PyTorch on the Apple GPU (MPS). It works, but it is
**much slower** than a large NVIDIA card. Tested on an M1 Ultra with 128 GB (Flame 2027.2):

| Clip | Time | Memory used |
|---|---|---|
| 1280×720, 49 frames | 11 min | 52 GB |
| 1920×1080, 33 frames | 19 min | 60 GB |
| 1920×1080, 73 frames | 48 min | 80 GB |

That is about 50 s per HD frame (a large NVIDIA GPU: about 1.6 s). It suits short shots,
lower resolutions or overnight runs.

- **Memory:** the GPU shares the Mac's memory and can use about 75% of it. With 128 GB, Full
  chunks (145 frames) at HD don't fit; **Chunk Size Auto** picks Half or Quarter for you.
  Going over the limit makes macOS swap to disk (everything slows down) rather than stop with
  an error.
- **Precision:** bf16. fp8 doesn't exist on the Apple GPU; choosing it shows a message. On Macs
  where it is faster (the M1 family), Setup also makes an **fp16 copy** of the main model
  (42 GB more disk, about 1.4× faster processing, same matte) and the plugin uses it by itself.
- **Apple's Command Line Tools** are required (the progress window and a GPU helper are
  compiled on first use). If they are missing, run `xcode-select --install` in Terminal.
- **Disk:** the same ~80 GB as on Linux, unless you link to existing model files (see
  [Existing Models](#3-choose-the-folders)), plus 42 GB for the fp16 copy where it is used.
- **Cancel** stops the job within a few seconds; the GPU may stay busy for a few seconds more
  while it finishes work it had already started.
- Only the M1 Ultra has been tested. Later chips (M2 and up) have faster bf16 and should do
  better; the plugin picks the faster setting by itself.

## How it works

Flame never waits on the model. The plugin is split into two parts:

1. A light **C++ OFX plugin** inside Flame. It shows the controls, looks up mattes in a disk
   cache, and displays them.
2. A **Python background process** on the same machine that loads the model and does the
   inference.

Processing never starts by itself. A node that was never processed passes its source through
with a white matte. Press **Process** and the plugin stages the clip and starts a separate job with
its own progress window and a Cancel button. When the job finishes, the viewer refreshes by
itself and shows the matte. Mattes are cached on disk per frame, so revisiting, re-rendering and
Batch Render are instant.

- The matte is delivered in the **alpha / matte output** of the node. The **image output** shows
  the source with the matte overlaid in magenta (30%), so you can judge the matte against the
  picture at a glance.
- Clips are processed in chunks (up to 145 frames; see **Chunk Size**) that overlap and are
  joined across the overlap (see **Merge**). Padding to the model's size and frame rules is
  automatic, and the output always matches the source size and frame count.
- **Watch the main object between chunks.** The model has no idea what you want matted: it picks
  the most prominent element of each chunk by itself. In a long clip, or with small chunks, a
  different object can become the most prominent in the next chunk, and the matte then switches
  subject at the seam. Merging cannot repair that. If it happens, use a larger Chunk Size, or give
  the model a region of interest (the picture's alpha, see **ROI**) so it only sees your subject.
- The model sees display-referred images. With **Input Colour** on Auto, Flame converts camera
  log and ACEScg footage for the model using each clip's colour tag. The image output then shows the
  picture in display space with the matte overlaid; the matte output is unchanged.
- Mattes are cached per source frame and settings. Changing Input Colour or Precision gives a
  new matte (`MATTE MISSING` until you process), and switching back finds the earlier one.
- **A processed node never passes through silently.** If its matte is missing (the cache was
  purged or swept, the setup moved to another machine, or the source changed upstream), both
  outputs show `MATTE MISSING` and the render fails, so a node deep in a setup gets noticed.
  Press Process to make the mattes again; **Purge Cache** returns the node to pass-through.

## Installation

Installation copies the plugin into Flame's OFX folder and adds a small Flame Python hook that
refreshes the viewer when a job finishes. The model, the LTX code and the Python runtime are
installed later, per user, from inside Flame (see [Setup](#setup)).

### From a release (recommended)

1. Download the release for your system from the GitHub releases page and unpack it:
   `LTXAlphaGen-<version>-rocky9.tar.gz` (Linux) or `LTXAlphaGen-<version>-macos-arm64.tar.gz`
   (macOS on Apple Silicon).
2. Quit Flame.
3. Run the installer with sudo (it writes to Flame's OFX folder):

   ```
   tar xzf LTXAlphaGen-0.2.1-rocky9.tar.gz     # or ...-macos-arm64.tar.gz
   cd LTXAlphaGen-0.2.1
   sudo ./install.sh
   ```

   It installs:
   - `/usr/OFX/Plugins/LTXAlphaGen.ofx.bundle` on Linux, `/Library/OFX/Plugins/LTXAlphaGen.ofx.bundle`
     on macOS (the plugin; an earlier version is replaced);
   - `/opt/Autodesk/shared/python/ltx_alphagen_hook.py` (the refresh hook). It will not replace
     a different file of that name.
4. Start Flame. In Batch, the node is under **LTX > LTX Alpha Gen**.

To uninstall, run `sudo ./install.sh --uninstall`, or delete the two items above. Your models,
settings and cache stay where Setup put them (see [Files and folders](#files-and-folders)).

### Building from source

You need Rocky Linux 9 (the plugin must link against glibc 2.34 or older and Flame's GCC 11
`libstdc++`), CMake and the system GCC 11:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=OFF
cmake --build build -j 8
cd build && ctest
```

The bundle is `build/LTXAlphaGen.ofx.bundle`. Copy or link it into `/usr/OFX/Plugins`, and copy
`flame_hook/ltx_alphagen_hook.py` into `/opt/Autodesk/shared/python`. A build with
`-DLTXAG_DEV_BUILD=ON` (the default) adds a Prototype page with test controls and lets you use
the node before Setup is complete.

## Setup

Setup runs once per user, from the node's **Setup** tab. Until it has finished, the node shows
`SETUP REQUIRED` on both its image and matte outputs, and Process is disabled.

### 1. Accept the licence

In the **Licence** column:

1. Press **View Licence** and read the LTX-2.x Community License.
2. Tick **I Accept**. This is stored per user. If a later version of the plugin ships a different
   licence text, you will be asked again.

### 2. Get access to the models on Hugging Face

The weights are in two gated Hugging Face repos. With your Hugging Face account, open both pages
and accept the terms:

- https://huggingface.co/Lightricks/LTX-2.5
- https://huggingface.co/Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen

Then create an access token (Settings > Access Tokens, type "Read"). You can skip this step if
all six model files are already on your system (see Existing Models below).

### 3. Choose the folders

In the **Models** column:

- **Model Folder**: where the models and the LTX runtime are installed (about 80 GB). Pick a fast
  local disk with enough space. Default: `~/ltx-alphagen-models`; on many Flame systems a data
  volume is a better choice than the home folder.
- **Existing Models** (optional): a folder that may already hold the model files, for example a
  ComfyUI models folder. Setup searches it by exact file name, checks each file's size and
  SHA-256 checksum against the list shipped with the plugin, and uses the matching files instead
  of downloading them. Files that don't match are ignored. When you press Download, it asks
  whether to **link** to them (saves up to 73 GB; the folder must then stay where it is and
  stay available, and on a network folder every model load is slower) or **copy** them into
  the Model Folder.

### 4. Download

Paste your token into **HF Token** and press **Download**. The token is passed to the download
once and never saved (the caption then reads "Token used HH:MM, not stored"). If everything is
found under Existing Models, you can leave the token empty.

A progress window shows each step, with Cancel:

1. **Runtime** (in `<Model Folder>/runtime`): the `uv` package manager, the Lightricks LTX-2
   code at a pinned commit, and its Python packages (about 6.6 GB, shown as "x of ~6.6 GB"),
   plus OpenColorIO. The package versions are pinned by a lock file shipped with the plugin, so
   every install is the same environment. Takes a few minutes.
2. **Models**: for each of the six files, either keep it (already in the Model Folder and
   checked before), copy or link it from Existing Models, or download it from Hugging Face. Every file is
   checked with its SHA-256 checksum while it is copied or downloaded. Downloads show "x of y GB"
   and speed.
3. **Prompt cache**: a one-time step that runs the Gemma text encoder once, so it never has to be
   loaded during processing.
4. **Settings**: the Model Folder and runtime are recorded, and the node becomes ready.

Running Download again is safe and quick: files already checked are kept. Network steps retry by
themselves. If Hugging Face refuses the download (HTTP 401/403), the message tells you which
repo's terms you still need to accept.

## Using it

1. Add **LTX Alpha Gen** after your source in Batch.
2. Leave **Input Colour** on Auto if the clip's colour space is tagged correctly in Flame (see
   below).
3. Press **Process**.
4. Watch the progress window, for example
   `Chunk 2/3 · frames 121–265 of 300 · step 5/8 · 3:12 elapsed · about 2:40 left`.
   The elapsed time ticks every second; the time left appears after the first two steps.
   **Cancel** stops the job and frees the GPU.
5. When the job finishes, the viewer steps one frame and back to refresh, and shows the matte.

While a job runs, the frames show `PROCESSING`. You can keep working in Flame, but avoid other
GPU-heavy work while processing.

**Before a job starts**, the plugin estimates the GPU memory the clip needs from its size and
frame count. If that is more than is free (minus the VRAM Reserve), it asks whether to continue,
and lists what Flame and other applications hold on the GPU. The estimate errs on the safe side.
If you continue and the GPU really runs out, the job stops with an out-of-memory error; Flame is
not affected.

## Controls

The node has two tabs: the first holds **Alpha Gen**, **Performance**, **Chunking**, **ROI** and
**Cache**; the
**Setup** tab holds **Licence**, **Models** and **Support**. Settings marked *per user* are
shared by all your nodes and projects; the others are saved with each node.

### Alpha Gen

- **Input Colour**: how the source reaches the model, which needs display-referred images.
  Only the model's input is converted.
  - `Auto` (default for new nodes): Flame converts the source to ACEScg from the clip's colour
    tag (any camera log, ACES or linear space Flame knows), and the plugin then applies Flame's
    ACES 2.0 view "ACES 2.0 - SDR 100 nits (Rec.709)" on display Rec.1886. Rec.709 video (and
    other display-referred tags) is used as it is. So the clip's tag in Flame must be right.
  - `Bypass`: no conversion; the source is already display-referred (Rec.709 / Rec.1886). Both outputs
    show the matte (no picture or overlay: without Flame's conversion the plugin can't show the picture
    in display space).

  A wrong tag gives a noticeably worse matte. Each setting, and with Auto each colour space
  Flame delivers, has its own cached mattes. Nodes saved with an older version keep working:
  Bypass stays Bypass, any other colour space becomes Auto (check the clip's tag).
- **Process**: generate mattes for the whole clip in the background. It always processes the
  whole clip connected to the node.
- **Status**: the state of this node's last job (processing, done, failed, cancelled).
- **Show Marker**: on frames without a matte, the matte output shows faint diagonal stripes with
  a caption: `NOT PROCESSED`, `PROCESSING`, `FAILED`, `CANCELLED`, `SETUP REQUIRED`,
  `NOT CONNECTED` or `NOT SUPPORTED`. Off: such frames are plain black. Frames with a matte are
  not affected. The image output shows the plain source on such frames, except
  `SETUP REQUIRED`, which shows on the image output as well.

### Performance

- **Precision**: `bf16` (reference quality) or `fp8` (about 17 GB less GPU memory, same speed).
  Each precision has its own cached mattes. The built-in default is bf16 if an HD 145-frame
  chunk fits in the free VRAM, otherwise fp8. Nothing is ever downgraded silently.
- **Unload GPU**: on (default): the model is unloaded when this node's job ends, and all GPU
  memory goes back to Flame. Off: the model stays loaded, so the next job skips loading it
  (measured: a small job took 11 s instead of 22 s) and each chunk saves ~6 s of setup. It costs
  more GPU memory and holds it between jobs. A loaded model unloads itself after 5 minutes without jobs, and when Flame
  quits. With several Alpha Gen nodes, turn Unload GPU off on all but the last node you process.
- **VRAM Reserve**: GPU memory (GB) kept free for Flame while processing. The default is 5% of
  the VRAM that was free when the plugin loaded.
- **Save Default** (*per user*): makes this node's Precision, Unload GPU and VRAM Reserve the
  defaults for every new node. Order of precedence: built-in default, then your saved default,
  then the node's own setting.

### Chunking

- **Chunk Size**: how many frames the model processes at once. `Auto` (default) takes the longest
  size that fits the free GPU memory (after the VRAM Reserve). `Full (145)`, `Half (73)` and
  `Quarter (33)` are fixed sizes (all of the form 8n+1, which the model needs); smaller ones need
  less GPU memory, which helps smaller cards, but add seams and give the model less context.
  The chunks overlap by 24, 16 and 12 frames respectively. Clips shorter than the chunk size are
  one chunk.
- **Merge**: how neighbouring chunks are joined across their overlap.
  - `Smooth` (default): an eased dissolve from one chunk's matte to the next.
  - `Flow Warp`: the two chunks' mattes are first warped towards each other with optical flow,
    so an edge that the chunks disagree on moves from one position to the other instead of
    showing a ghost. It takes a few seconds per seam, small next to the model time. It does not
    fix a switch of the main object (see How it works).

### Motion

- **Frame Hold**: for fast motion. The model packs 8 frames into each of its time steps, so an
  edge that moves a long way within 8 frames can come out soft, lagging or jittery. `2x`, `4x`
  and `8x` repeat each frame so a time step holds 4, 2 or 1 source frames; the copies are
  dropped again, so the output has the clip's own frames. In our tests (synthetic motion of
  24–128 pixels per frame) `4x` removed most of the edge error and `8x` added little. On longer
  clips processing takes about 2, 4 and 8 times as long, and a chunk holds fewer frames (Full:
  73, 37, 19), so a clip has more seams. `Auto` measures the motion while Process reads the
  frames and holds only the chunks that need it (2x from about 12 pixels per frame, 4x from
  about 40); slow parts of the clip run at normal speed. With an ROI alpha, Auto looks at the
  motion inside it, so a rough garbage matte around the subject is enough. Changing the setting
  needs a new Process; the mattes of each setting are cached separately. `Off` (default) is
  right for most shots.
- **Version**: the plugin's version and build, shown for support.

### ROI (Alpha)

Optional. Put your subject's matte in the **alpha channel of the picture** that goes into the
node (it does not need to be exact; a loose garbage matte is fine) to tell the model where to look.
There is no separate input and no switch: if the alpha is fully opaque in every frame, there is
no ROI and the whole frame is processed. If the alpha is a matte, the plugin takes its bounding
box over the **whole clip**, adds a margin, and shows the model only that crop. The result is
placed back into a black full-size frame, so the output always matches the source. Besides
steering the model, a small crop needs far less GPU memory and time. The first field of the
column shows what the last Process did (`Crop 800x1056` or `None (alpha opaque)`).

- **ROI Margin (px)**: pixels of context around the matte's box (default 64, at least 32), so the
  model can find fine edges the matte missed. The crop is also at least 512 pixels per side, and
  its sides are multiples of 32.
- **Black Out Outside ROI** (off by default): black out everything outside the alpha matte before
  the model sees it. A stronger cue than the crop alone; dilate the matte upstream if it is tight.
- **Multiply Result by ROI** (off by default): multiply the result by the alpha matte. Applied
  when viewing, so you can toggle it without reprocessing.

The alpha is read when you press **Process**. If you change the matte, ROI Margin or Black Out
Outside ROI, press Process again (the marker shows until then). Mattes made with an ROI are cached
separately from those without.

### Cache

- **Cache Folder** (*per user*): where mattes are stored. Default `~/.cache/ltx-alphagen/mattes`.
- **Cache Size**: how much the cache holds.
- **Purge Cache**: delete the cached mattes of this node's clip for its current Input Colour
  and Precision (mattes made with other settings stay).
- **Purge All**: delete every cached matte of every node and project.
- Old mattes are also swept automatically when Flame starts: unused for 30 days, or least
  recently used first when the cache passes 50 GB.

### Licence (Setup tab)

- **View Licence**, **I Accept** (*per user*), **HF Token** and **Download**: see
  [Setup](#setup).

### Models (Setup tab)

- **Model Folder** (*per user*), **Existing Models** (*per user*): see [Setup](#setup).
- **Setup**: whether the setup is complete.

### Support (Setup tab)

- **Debug Logging** (*per user*): more detail in the log, including every render. Turn it on when
  you are chasing a problem.
- **Collect Logs**: writes `~/ltx-alphagen-support-<date>.tar.gz` with the logs, a system
  summary (OS, GPU, driver, memory, plugin version) and the job records. Review it before you
  send it: it contains file paths, but no images, no clip or project names, and no token.

## Files and folders

| What | Where |
|---|---|
| Plugin | `/usr/OFX/Plugins/LTXAlphaGen.ofx.bundle` |
| Refresh hook | `/opt/Autodesk/shared/python/ltx_alphagen_hook.py` |
| Models and LTX runtime | your Model Folder (`runtime/` holds the code and Python packages) |
| Settings (per user) | `~/.config/ltx-alphagen/config.conf` |
| Matte cache | your Cache Folder (default `~/.cache/ltx-alphagen/mattes`) |
| Logs | `~/.cache/ltx-alphagen/logs/` (`ltx-alphagen.log`, `vram_stats.csv`) |
| Frames during a job | `/dev/shm` (removed when the job ends) |

## Licensing

- **This plugin** is Copyright (c) 2026 Jan Klier, donated to Logik.tv, and licensed under the **Mozilla Public License 2.0**
  (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).
  The vendored OpenFX headers in `third_party/openfx` keep their own BSD licence.
- **LTX code and weights are not part of this repository** and are never bundled. Setup
  downloads them from Lightricks' own repositories. They are under the **LTX-2.x Community
  License**, which you accept on the Setup tab, and you are responsible for your own compliance
  with it.
- In particular, **entities with USD 10M or more in annual revenue need a paid licence from
  Lightricks.**
- Output is AI-generated content. Check the LTX licence for disclosure requirements.
- The Python packages Setup installs (PyTorch, NVIDIA CUDA libraries, OpenColorIO and others)
  come from their public sources under their own licences.

This is not legal advice, and the licensing notes will be reviewed before a public release.

## Status and roadmap

**Works today:** the plugin in Flame with the real Alpha Gen worker, on Linux (NVIDIA) and on
macOS (Apple Silicon, slower); guided Setup (runtime
install, Hugging Face download, reuse of existing model files, checksum verification); input
colour conversion; bf16 and fp8; a resident model with Unload GPU; a clip-size-aware memory check
with a warning; a progress window with time left; automatic viewer refresh; the disk matte cache;
chunking for long clips with selectable chunk size (including Auto for smaller GPUs) and merge
mode (Smooth or optical-flow Flow Warp); a region of interest taken from the picture's alpha;
logging and Collect Logs.

### On the roadmap

- Disk staging for very long clips. The whole clip is staged in `/dev/shm` (about 12.5 MB per HD
  frame), which limits very long or very large clips.
- Processing only the frames that are not cached yet. Process always processes the whole clip.

## Support and logs

Everything is logged to `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`. The HF token, image data
and clip or project names are never logged. Use **Collect Logs** to create a bundle for a bug
report.

Each job also adds a line to `~/.cache/ltx-alphagen/logs/vram_stats.csv` with the estimated and
the actual GPU memory use. Sending it with a bug report helps improve the estimate for GPUs we
haven't measured.

## Contributing

Issues and pull requests are welcome. 

