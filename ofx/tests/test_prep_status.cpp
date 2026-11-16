// Plain-assert tests for prep_status (decision 79); run via ctest.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "prep_status.h"

using namespace ltxag;

static int failures = 0;
#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                        \
      std::fprintf(stderr, "\n");                               \
      ++failures;                                               \
    }                                                           \
  } while (0)

static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

int main() {
  CHECK(prepStatusText(0.5, "Reading") == "0.5000\nReading\n", "status text");
  CHECK(prepStatusText(-1, "x") == "0.0000\nx\n" && prepStatusText(3, "x") == "1.0000\nx\n", "clamped");
  CHECK(prepClock(0) == "0:00" && prepClock(65.4) == "1:05" && prepClock(3725) == "1:02:05", "clock %s",
        prepClock(3725).c_str());

  // Pass 0 of 100 frames, 10 done at 2 s/frame: 90 + 100 frames left -> 6:20.
  PrepLine l = prepFrameLine(0, 10, 100, 2.0);
  CHECK(std::abs(l.frac - 0.05) < 1e-9, "frac %f", l.frac);
  CHECK(l.text == "Scanning the source alpha (ROI): frame 10/100 · about 6:20 left", "%s", l.text.c_str());
  l = prepFrameLine(1, 50, 100, 0.5);
  CHECK(std::abs(l.frac - 0.75) < 1e-9, "frac %f", l.frac);
  CHECK(l.text == "Reading frames: 50/100 · about 0:25 left", "%s", l.text.c_str());
  CHECK(prepFrameLine(1, 0, 100, 0.0).text == "Reading frames: 0/100", "no estimate yet");
  CHECK(prepFrameLine(0, 0, 0, 1.0).frac == 0.0, "empty clip");

  // Files: atomic status, rate limit, cancel marker, close removes everything.
  char tmpl[] = "/tmp/ltxag_prep_test_XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  {
    std::ofstream(dir + "/ltxag_42_prep.cancel") << "stale\n";
    PrepStatus p(dir, 42);
    CHECK(p.statusPath() == dir + "/ltxag_42_prep.txt", "%s", p.statusPath().c_str());
    CHECK(!exists(p.cancelPath()) && !p.cancelled(), "a stale cancel marker is removed");
    p.set(0.1, "first");
    CHECK(slurp(p.statusPath()) == "0.1000\nfirst\n", "first write");
    p.set(0.2, "too soon");
    CHECK(slurp(p.statusPath()) == "0.1000\nfirst\n", "rate limited");
    p.set(0.3, "forced", true);
    CHECK(slurp(p.statusPath()) == "0.3000\nforced\n", "force");
    CHECK(!exists(dir + "/ltxag_42_prep.tmp"), "no temp file left");
    std::ofstream(p.cancelPath()) << "cancel\n";
    CHECK(p.cancelled(), "cancel seen");
    p.close();
    CHECK(!exists(p.statusPath()) && !exists(p.cancelPath()) && !p.cancelled(), "close removes both");
    p.set(0.9, "after close", true);
    CHECK(!exists(p.statusPath()), "no write after close");
    PrepStatus q(dir, 43);
    q.set(0.5, "x");
  }  // destructor closes
  CHECK(!exists(dir + "/ltxag_43_prep.txt"), "destructor removes the status file");
  rmdir(dir.c_str());
  CHECK(!exists(dir), "test folder left non-empty");

  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
