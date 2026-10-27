"""Input colour conversion for the model (decision 54: OCIO runs in the worker, not the shim).

Ported from ../ComfyUI_OpenClip (nodes/colour_transform.py, lib/colour_transform.py), which
was verified against Flame's ACES 2.0 config:
  - config: $OCIO if set, else the newest Flame
    /opt/Autodesk/colour_mgmt/configs/flame_configs/<version>/aces2.0_config/config.ocio
  - transform: DisplayViewTransform(src, display, view), NOT config.getProcessor(src, display).
    A display colour space in an OCIO v2/ACES config is encoding-only; the tone mapping lives on
    the View. The plain path gave out-of-range values (ACEScg (2.0,0.05,0.05) -> (1.66,-0.52,0.09)
    instead of (1.00,0.28,0.31)).
Display and view are fixed (decision 28). Source names verified in Flame's 2026.0 config
(LogC3 and BMD Film Gen5 added 2026-10-11; that config only has LogC3 at EI800).
Input Colour Auto (docs/colour-auto.md): the plugin asks Flame for the source in ACEScg and
passes the colourspace Flame actually delivered ("colour_tag"). ACEScg (or any other scene-
referred tag, if Flame ignored the request) is the OCIO source of the same view transform; a
display-referred or data tag (Rec.709 video, sRGB, Matte, ...) or no tag at all is used as it is,
like Bypass.
applyRGB releases the GIL: one thread per frame is ~10x faster with identical output
(reference machine, 2K: 820 -> 83 ms/frame with 16 threads).
"""

from __future__ import annotations

import glob
import os
from concurrent.futures import ThreadPoolExecutor

import numpy as np

DISPLAY = "Rec.1886 Rec.709 - Display"
VIEW = "ACES 2.0 - SDR 100 nits (Rec.709)"
FLAME_GLOB = "/opt/Autodesk/colour_mgmt/configs/flame_configs/*/aces2.0_config/config.ocio"

THREADS = min(16, os.cpu_count() or 1)

# Labels of the old plugin dropdown (until 0.2.1) -> OCIO source colour space (decision 27).
# "Bypass" = no conversion. Still accepted (manifests, smoke tools, --colour); the plugin now sends
# "Auto" or "Bypass" plus the delivered tag.
AUTO = "Auto"
# Display-referred tags Flame uses for Rec.709 video (name and aliases in Flame's 2026.0 config):
# no conversion, and no OCIO needed to decide that.
REC709_DISPLAY_TAGS = {DISPLAY, "Rec.709 video", "rec1886_rec709_display", "g24_rec709_display"}
SOURCES = {
    "Bypass": None,
    "ACEScg": "ACEScg",
    "LogC4": "ARRI LogC4",
    "Log3G10": "Log3G10 REDWideGamutRGB",
    "LogC3": "ARRI LogC3 (EI800)",
    "BMD Film Gen5": "BMDFilm WideGamut Gen5",
}


def _version_key(path: str) -> tuple[int, ...]:
    try:
        return tuple(int(x) for x in os.path.basename(os.path.dirname(os.path.dirname(path))).split("."))
    except ValueError:
        return (0,)


def default_config() -> str:
    """$OCIO, else the newest Flame ACES 2.0 config, else ''."""
    env = os.environ.get("OCIO", "").strip()
    if env:
        return env
    found = glob.glob(FLAME_GLOB)
    return max(found, key=_version_key) if found else ""


class Converter:
    """Source colour space -> Rec.1886 display values through the fixed ACES 2.0 SDR view.

    label: "Auto" (the source is in `tag`, the colourspace the host delivered), "Bypass", or an
    old dropdown label (SOURCES)."""

    def __init__(self, label: str, config_path: str = "", tag: str = "") -> None:
        if label != AUTO and label not in SOURCES:
            raise ValueError(f"unknown input colour '{label}' (expected Auto or one of {', '.join(SOURCES)})")
        self.label, self.tag = label, tag
        self.config_path = ""
        self.reason = ""  # why Auto does not convert
        self._cpu = None
        if label == AUTO:
            self.source = SOURCES.get(tag) or tag or None  # an old label as a tag maps as before
            if not tag:
                self.reason = "the host gave no colourspace tag"
            elif tag in REC709_DISPLAY_TAGS:
                self.reason = f"'{tag}' is display-referred"
            if self.reason:
                self.source = None
                return
        else:
            self.source = SOURCES[label]
            if self.source is None:
                return
        import PyOpenColorIO as ocio  # in the LTX venv; only needed when converting

        self.config_path = config_path or default_config()
        if not self.config_path:
            raise ValueError("no OCIO config: set $OCIO or install Flame's ACES 2.0 config")
        config = ocio.Config.CreateFromFile(self.config_path)
        cs = config.getColorSpace(self.source)
        if cs is None:
            raise ValueError(f"colour space '{self.source}' is not in {self.config_path}")
        if label == AUTO and (cs.isData() or cs.getReferenceSpaceType() == ocio.REFERENCE_SPACE_DISPLAY):
            self.reason = f"'{tag}' is display-referred or data"
            self.source = None
            return
        t = ocio.DisplayViewTransform()
        t.setSrc(self.source)
        t.setDisplay(DISPLAY)
        t.setView(VIEW)
        self._cpu = config.getProcessor(t, ocio.TRANSFORM_DIR_FORWARD).getDefaultCPUProcessor()

    @property
    def active(self) -> bool:
        return self._cpu is not None

    def apply(self, frames: np.ndarray) -> np.ndarray:
        """(N,H,W,3) any float dtype -> float32 display-referred values (new array)."""
        arr = np.array(frames, dtype=np.float32, copy=True)
        if self._cpu is not None:
            def one(i: int) -> None:
                self._cpu.applyRGB(arr[i].reshape(-1, 3))  # in place on the contiguous copy

            with ThreadPoolExecutor(THREADS) as ex:
                list(ex.map(one, range(arr.shape[0])))
        return arr

    def describe(self) -> str:
        if self._cpu is None:
            if self.label == AUTO:
                return f"input colour: Auto, {self.reason}: no conversion (frames used as display-referred)"
            return "input colour: Bypass (frames used as display-referred)"
        prefix = f"Auto (delivered '{self.tag}'): " if self.label == AUTO else ""
        return f"input colour: {prefix}{self.source} -> {DISPLAY} / {VIEW} (config {self.config_path})"
