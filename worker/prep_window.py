"""The "preparing" window (decision 79), launched detached by Process before it fetches the clip.

  python -I -B prep_window.py <status file> <cancel file> <flame pid> [title]

Process reads every source frame twice on Flame's UI thread (ROI scan, then staging) before
job.py starts; at UHD that takes minutes with Flame frozen. This window is its own process, so it
paints meanwhile. The plugin (ofx/src/prep_status.h) writes the status file, "<fraction>\\n<text>\\n",
replaced atomically, and removes it when it is done (job launched, refused or cancelled). Cancel
writes the cancel file; the plugin checks it between frames. Standard library only, like job.py's
OS layer: no window (no zenity, no UI) = exit at once.
"""

from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ltxag_worker import oslayer  # noqa: E402


def read_status(path: str) -> tuple[float, str] | None:
    """(fraction, text) from the plugin's status file; None once it is gone (the plugin is done)."""
    try:
        with open(path, encoding="utf-8") as f:
            first, _, text = f.read().partition("\n")
    except OSError:
        return None
    try:
        frac = float(first)
    except ValueError:
        frac = 0.0
    return frac, text.strip()


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def run(win, status: str, cancel: str, flame_pid: int, poll: float = 0.1) -> str:
    """Mirrors the status file into the window until the plugin is done, Flame is gone or the
    user pressed Cancel. Returns which of the three ("done", "flame gone", "cancelled")."""
    shown = None
    while True:
        st = read_status(status)
        if st is None:
            win.close()
            return "done"
        if not pid_alive(flame_pid):
            win.close()
            return "flame gone"
        if win.gone():
            if os.path.exists(status):  # not a Cancel that raced the plugin finishing
                with open(cancel, "w") as f:
                    f.write("cancel\n")
            return "cancelled"
        if st != shown:
            try:
                win.show(*st)
                shown = st
            except BrokenPipeError:
                pass  # the window closed: gone() says so on the next round
        time.sleep(poll)


def main() -> int:
    status, cancel, flame_pid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    title = sys.argv[4] if len(sys.argv) > 4 else "LTX Alpha Gen"
    win = oslayer.ProgressWindow(title, 560, True)
    if not win.shown:
        return 0
    run(win, status, cancel, flame_pid)
    return 0


if __name__ == "__main__":
    sys.exit(main())
