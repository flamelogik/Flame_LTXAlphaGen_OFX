"""Input Colour (colour.Converter): Auto with the delivered tag, Bypass, the old labels.

The OCIO paths run against a fake PyOpenColorIO (the worker tests' python has none); with the
real PyOpenColorIO and Flame's ACES 2.0 config (the LTX venv on a Flame machine) RealOcioTest
checks the numbers too.
"""

import os
import sys
import types
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ltxag_worker import colour  # noqa: E402


def fake_ocio(spaces):
    """A PyOpenColorIO stand-in: spaces = {name: (display_referred, is_data)}; records setSrc."""
    m = types.ModuleType("PyOpenColorIO")
    m.REFERENCE_SPACE_SCENE, m.REFERENCE_SPACE_DISPLAY, m.TRANSFORM_DIR_FORWARD = "scene", "display", "fwd"
    m.sources = []

    class Space:
        def __init__(self, display, data):
            self.display, self.data = display, data

        def isData(self):
            return self.data

        def getReferenceSpaceType(self):
            return m.REFERENCE_SPACE_DISPLAY if self.display else m.REFERENCE_SPACE_SCENE

    class Cpu:
        def applyRGB(self, a):
            a *= 0.5  # any visible change

    class Config:
        @staticmethod
        def CreateFromFile(path):
            return Config()

        def getColorSpace(self, name):
            return Space(*spaces[name]) if name in spaces else None

        def getProcessor(self, t, direction):
            m.sources.append((t.src, t.display, t.view))
            return types.SimpleNamespace(getDefaultCPUProcessor=Cpu)

    class DisplayViewTransform:
        def setSrc(self, s):
            self.src = s

        def setDisplay(self, d):
            self.display = d

        def setView(self, v):
            self.view = v

    m.Config, m.DisplayViewTransform = Config, DisplayViewTransform
    return m


SPACES = {"ACEScg": (False, False), "ARRI LogC3 (EI800)": (False, False), colour.DISPLAY: (True, False),
          "sRGB - Display": (True, False), "Matte": (True, False), "Raw": (False, True)}


class FakeOcioTest(unittest.TestCase):
    def setUp(self):
        self.saved = sys.modules.get("PyOpenColorIO")
        self.ocio = sys.modules["PyOpenColorIO"] = fake_ocio(SPACES)
        self.frames = np.full((2, 4, 4, 3), 0.8, dtype=np.float16)

    def tearDown(self):
        if self.saved is None:
            sys.modules.pop("PyOpenColorIO", None)
        else:
            sys.modules["PyOpenColorIO"] = self.saved

    def assertUnchanged(self, c):
        self.assertFalse(c.active)
        np.testing.assert_array_equal(c.apply(self.frames), self.frames.astype(np.float32))

    def test_auto_acescg_runs_the_view_transform(self):
        c = colour.Converter("Auto", "/fake/config.ocio", "ACEScg")
        self.assertTrue(c.active)
        self.assertEqual(self.ocio.sources, [("ACEScg", colour.DISPLAY, colour.VIEW)])
        np.testing.assert_allclose(c.apply(self.frames), 0.4, atol=1e-3)
        self.assertIn("Auto (delivered 'ACEScg')", c.describe())

    def test_auto_same_transform_as_old_acescg_label(self):
        colour.Converter("Auto", "/fake/config.ocio", "ACEScg")
        colour.Converter("ACEScg", "/fake/config.ocio")
        self.assertEqual(self.ocio.sources[0], self.ocio.sources[1])

    def test_auto_rec709_display_tag_no_conversion(self):
        for tag in (colour.DISPLAY, "Rec.709 video"):
            c = colour.Converter("Auto", "/fake/config.ocio", tag)
            self.assertUnchanged(c)
            self.assertIn("no conversion", c.describe())
        self.assertEqual(self.ocio.sources, [])

    def test_auto_other_display_or_data_tag_no_conversion(self):
        for tag in ("sRGB - Display", "Matte", "Raw"):
            self.assertUnchanged(colour.Converter("Auto", "/fake/config.ocio", tag))
        self.assertEqual(self.ocio.sources, [])

    def test_auto_logc3_fallback_uses_the_delivered_tag(self):
        # Flame ignored the request: the delivered tag is the OCIO source.
        c = colour.Converter("Auto", "/fake/config.ocio", "ARRI LogC3 (EI800)")
        self.assertTrue(c.active)
        self.assertEqual(self.ocio.sources, [("ARRI LogC3 (EI800)", colour.DISPLAY, colour.VIEW)])

    def test_auto_unknown_tag_is_an_error(self):
        with self.assertRaises(ValueError):
            colour.Converter("Auto", "/fake/config.ocio", "No Such Space")

    def test_old_labels_still_map(self):
        colour.Converter("LogC3", "/fake/config.ocio")
        self.assertEqual(self.ocio.sources[0][0], "ARRI LogC3 (EI800)")


class NoOcioTest(unittest.TestCase):
    """Paths that must not need PyOpenColorIO at all."""

    def setUp(self):
        self.saved = sys.modules.get("PyOpenColorIO")
        sys.modules["PyOpenColorIO"] = None  # import fails

    def tearDown(self):
        if self.saved is None:
            sys.modules.pop("PyOpenColorIO", None)
        else:
            sys.modules["PyOpenColorIO"] = self.saved

    def test_bypass_ignores_the_tag(self):
        c = colour.Converter("Bypass", "", "ACEScg")
        self.assertFalse(c.active)
        self.assertIn("Bypass", c.describe())

    def test_auto_without_tag_no_conversion(self):
        c = colour.Converter("Auto")
        self.assertFalse(c.active)
        self.assertIn("no colourspace tag", c.describe())

    def test_auto_rec709_without_ocio(self):
        self.assertFalse(colour.Converter("Auto", "", colour.DISPLAY).active)

    def test_unknown_mode(self):
        with self.assertRaises(ValueError):
            colour.Converter("auto")


def real_ocio():
    try:
        import PyOpenColorIO  # noqa: F401
    except ImportError:
        return False
    return bool(colour.default_config())


@unittest.skipUnless(real_ocio(), "needs PyOpenColorIO and Flame's ACES 2.0 config")
class RealOcioTest(unittest.TestCase):
    def test_numbers(self):
        px = np.array([[[[2.0, 0.05, 0.05]]]], dtype=np.float32)
        auto = colour.Converter("Auto", "", "ACEScg").apply(px)
        np.testing.assert_array_equal(auto, colour.Converter("ACEScg").apply(px))  # model input unchanged
        np.testing.assert_allclose(auto[0, 0, 0], [1.00, 0.28, 0.31], atol=0.01)  # colour.py docstring
        logc3 = colour.Converter("Auto", "", "ARRI LogC3 (EI800)").apply(px)
        np.testing.assert_array_equal(logc3, colour.Converter("LogC3").apply(px))
        for tag in (colour.DISPLAY, "sRGB - Display", "Matte", "Raw"):
            np.testing.assert_array_equal(colour.Converter("Auto", "", tag).apply(px), px, tag)


if __name__ == "__main__":
    unittest.main()
