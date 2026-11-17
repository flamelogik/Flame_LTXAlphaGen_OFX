// The "preparing" window (decision 79): process() fetches every source frame twice on Flame's UI
// thread (ROI scan, then staging) before job.py and its progress window start, minutes at UHD. A
// small detached process (worker/prep_window.py) shows a progress window meanwhile; this side
// writes what it shows into a status file and reads its Cancel back from a marker file.
//
// Files in stagingDir(), named ltxag_<flame pid>_... so the stale-staging sweep removes them once
// Flame is gone (cache.cpp); one process() at a time (78), so one pair per Flame:
//   ltxag_<pid>_prep.txt     "<fraction>\n<text>\n", replaced atomically (rename); gone = done
//   ltxag_<pid>_prep.cancel  written by the window when the user pressed Cancel
#pragma once

#include <chrono>
#include <string>

namespace ltxag {

// Status file content.
std::string prepStatusText(double frac, const std::string& text);

// "m:ss" or "h:mm:ss", like job.py's mmss().
std::string prepClock(double seconds);

// Progress over the two passes of a clip of n frames: pass 0 = ROI scan, pass 1 = reading the
// frames for the job; done = frames finished in this pass; secPerFrame <= 0: no time left yet.
// The time left assumes pass 1 costs per frame what pass 0 did (both fetch every frame).
struct PrepLine {
  double frac;
  std::string text;
};
PrepLine prepFrameLine(int pass, int done, int n, double secPerFrame);

class PrepStatus {
 public:
  PrepStatus(const std::string& dir, int pid);  // removes a stale cancel marker
  ~PrepStatus() { close(); }
  const std::string& statusPath() const { return status_; }
  const std::string& cancelPath() const { return cancel_; }
  // Writes the status file; at most ~4 times a second unless force.
  void set(double frac, const std::string& text, bool force = false);
  bool cancelled() const;  // the window's Cancel was pressed
  void close();            // removes both files: the window closes itself

 private:
  std::string status_, tmp_, cancel_;
  std::chrono::steady_clock::time_point last_{};
  bool written_ = false, closed_ = false;
};

}  // namespace ltxag
