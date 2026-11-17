#include "prep_status.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>

namespace ltxag {

std::string prepStatusText(double frac, const std::string& text) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4f\n", frac < 0.0 ? 0.0 : frac > 1.0 ? 1.0 : frac);
  return buf + text + "\n";
}

std::string prepClock(double seconds) {
  const long s = std::lround(seconds < 0.0 ? 0.0 : seconds);
  char buf[32];
  if (s >= 3600)
    std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, s % 3600 / 60, s % 60);
  else
    std::snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
  return buf;
}

PrepLine prepFrameLine(int pass, int done, int n, double secPerFrame) {
  const std::string count = std::to_string(done) + "/" + std::to_string(n);
  PrepLine l;
  l.frac = n > 0 ? (double(pass) * n + done) / (2.0 * n) : 0.0;
  l.text = pass == 0 ? "Scanning the source alpha (ROI): frame " + count : "Reading frames: " + count;
  if (secPerFrame > 0.0)
    l.text += " · about " + prepClock(secPerFrame * ((n - done) + (pass == 0 ? n : 0))) + " left";
  return l;
}

PrepStatus::PrepStatus(const std::string& dir, int pid) {
  const std::string base = dir + "/ltxag_" + std::to_string(pid) + "_prep";
  status_ = base + ".txt", tmp_ = base + ".tmp", cancel_ = base + ".cancel";
  unlink(cancel_.c_str());
}

void PrepStatus::set(double frac, const std::string& text, bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (closed_ || (!force && written_ && now - last_ < std::chrono::milliseconds(250))) return;
  {
    std::ofstream f(tmp_, std::ios::trunc);
    f << prepStatusText(frac, text);
    if (!f) return;
  }
  if (std::rename(tmp_.c_str(), status_.c_str()) == 0) last_ = now, written_ = true;
}

bool PrepStatus::cancelled() const { return !closed_ && access(cancel_.c_str(), F_OK) == 0; }

void PrepStatus::close() {
  if (closed_) return;
  closed_ = true;
  unlink(status_.c_str());
  unlink(tmp_.c_str());
  unlink(cancel_.c_str());
}

}  // namespace ltxag
