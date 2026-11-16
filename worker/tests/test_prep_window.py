"""The "preparing" window (decision 79): status file protocol, Cancel, end conditions."""

import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import prep_window  # noqa: E402


class FakeWindow:
    def __init__(self) -> None:
        self.lines, self.closed, self.cancel = [], False, False

    def gone(self) -> bool:
        return self.cancel

    def show(self, frac: float, text: str) -> None:
        self.lines.append((frac, text))

    def close(self, wait: float = 0.0) -> None:
        self.closed = True


def read(path: str) -> str:
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return ""


def write_status(path: str, frac: float, text: str) -> None:
    with open(path + ".tmp", "w") as f:  # as the plugin: replace atomically
        f.write(f"{frac:.4f}\n{text}\n")
    os.replace(path + ".tmp", path)


class PrepWindowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.mkdtemp()
        self.status = os.path.join(self.dir, "ltxag_1_prep.txt")
        self.cancel = os.path.join(self.dir, "ltxag_1_prep.cancel")

    def tearDown(self) -> None:
        shutil.rmtree(self.dir)

    def test_read_status(self) -> None:
        self.assertIsNone(prep_window.read_status(self.status))
        write_status(self.status, 0.25, "Reading frames: 5/20 · about 0:10 left")
        self.assertEqual(prep_window.read_status(self.status), (0.25, "Reading frames: 5/20 · about 0:10 left"))
        with open(self.status, "w") as f:
            f.write("junk")
        self.assertEqual(prep_window.read_status(self.status), (0.0, ""))

    def run_bg(self, win, pid=None):
        out = {}
        t = threading.Thread(target=lambda: out.setdefault(
            "r", prep_window.run(win, self.status, self.cancel, pid or os.getpid(), poll=0.01)))
        t.start()
        return t, out

    def test_mirrors_then_closes_when_the_file_goes(self) -> None:
        write_status(self.status, 0.1, "a")
        win = FakeWindow()
        t, out = self.run_bg(win)
        time.sleep(0.1)
        write_status(self.status, 0.6, "b")
        time.sleep(0.1)
        os.unlink(self.status)
        t.join(5)
        self.assertEqual(out["r"], "done")
        self.assertTrue(win.closed)
        self.assertEqual(win.lines, [(0.1, "a"), (0.6, "b")])  # unchanged content is not resent
        self.assertFalse(os.path.exists(self.cancel))

    def test_cancel_writes_the_marker(self) -> None:
        write_status(self.status, 0.1, "a")
        win = FakeWindow()
        t, out = self.run_bg(win)
        time.sleep(0.05)
        win.cancel = True
        t.join(5)
        self.assertEqual(out["r"], "cancelled")
        self.assertTrue(os.path.exists(self.cancel))

    def test_cancel_after_the_plugin_finished_writes_nothing(self) -> None:
        win = FakeWindow()
        win.cancel = True
        self.assertEqual(prep_window.run(win, self.status, self.cancel, os.getpid(), poll=0.01), "done")
        self.assertFalse(os.path.exists(self.cancel))

    def test_flame_gone(self) -> None:
        p = subprocess.Popen(["/bin/sleep", "0"])
        p.wait()
        write_status(self.status, 0.1, "a")
        win = FakeWindow()
        self.assertEqual(prep_window.run(win, self.status, self.cancel, p.pid, poll=0.01), "flame gone")
        self.assertTrue(win.closed)


@unittest.skipUnless(sys.platform.startswith("linux"), "zenity stand-in on PATH (Linux)")
class PrepWindowProcessTest(unittest.TestCase):
    """The script as the plugin launches it, with systest/fake_ui.py as zenity."""

    def test_end_to_end(self) -> None:
        d = tempfile.mkdtemp()
        try:
            bin_dir = os.path.join(d, "bin")
            os.makedirs(bin_dir)
            fake = os.path.join(HERE, "..", "..", "systest", "fake_ui.py")
            with open(os.path.join(bin_dir, "zenity"), "w") as f:  # this python, not the shebang's
                f.write(f"#!/bin/sh\nexec {sys.executable} -I -B {os.path.abspath(fake)} \"$@\"\n")
            os.chmod(os.path.join(bin_dir, "zenity"), 0o755)
            status, cancel = os.path.join(d, "s.txt"), os.path.join(d, "s.cancel")
            write_status(status, 0.3, "Scanning the source alpha (ROI): frame 3/10")
            env = dict(os.environ, PATH=bin_dir + os.pathsep + os.environ.get("PATH", ""), LTXAG_FAKE_UI_DIR=d)
            script = os.path.join(HERE, "..", "prep_window.py")
            p = subprocess.Popen([sys.executable, "-I", "-B", script, status, cancel, str(os.getpid()), "T"],
                                 env=env)
            deadline = time.time() + 10
            log = os.path.join(d, "progress.log")
            while time.time() < deadline and "frame 3/10" not in read(log):
                time.sleep(0.05)
            self.assertEqual(read(log).splitlines()[:2], ["# Scanning the source alpha (ROI): frame 3/10", "30"])
            open(os.path.join(d, "cancel"), "w").close()  # the user presses Cancel
            self.assertEqual(p.wait(10), 0)
            self.assertTrue(os.path.exists(cancel))
        finally:
            shutil.rmtree(d)


if __name__ == "__main__":
    unittest.main()
