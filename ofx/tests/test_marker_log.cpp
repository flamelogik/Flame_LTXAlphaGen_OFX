// Tests for the unprocessed-frame marker and the log line format / rotation.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "log.h"
#include "marker.h"

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

static void countValues(const std::vector<float>& m, size_t& stripe, size_t& text, size_t& other) {
  stripe = text = other = 0;
  for (float v : m) {
    if (v == kMarkerStripe) ++stripe;
    else if (v == kMarkerText) ++text;
    else if (v != 0.0f) ++other;
  }
}

int main() {
  // Marker: faint stripes everywhere, readable text in the middle, nothing else.
  for (auto wh : {std::make_pair(1920, 1080), std::make_pair(6144, 3240), std::make_pair(64, 36),
                  std::make_pair(3, 2), std::make_pair(1, 1)}) {
    std::vector<float> m;
    drawMarker(m, wh.first, wh.second, "NOT PROCESSED");
    CHECK(m.size() == size_t(wh.first) * wh.second, "size %dx%d", wh.first, wh.second);
    size_t stripe, text, other;
    countValues(m, stripe, text, other);
    CHECK(other == 0, "unexpected values %dx%d", wh.first, wh.second);
    if (wh.first >= 64)
      CHECK(stripe > m.size() / 20, "too few stripe pixels %dx%d", wh.first, wh.second);
    if (wh.first >= 256) {  // smaller frames are too small for a readable caption
      CHECK(text > 0, "no text %dx%d", wh.first, wh.second);
    }
  }
  {
    std::vector<float> m;
    drawMarker(m, 1920, 1080, "SETUP REQUIRED");  // every caption letter must be in the font
    size_t s, t, o;
    countValues(m, s, t, o);
    std::vector<float> n;
    drawMarker(n, 1920, 1080, "SETUP  EQUIRED");  // one glyph fewer
    size_t s2, t2, o2;
    countValues(n, s2, t2, o2);
    CHECK(t > t2, "'R' not rendered");
    for (const char* cap : {"PROCESSING", "FAILED", "CANCELLED"}) {
      std::vector<float> k;
      drawMarker(k, 1920, 1080, cap);
      countValues(k, s, t, o);
      CHECK(t > 0, "caption %s rendered nothing", cap);
    }
  }

  // Overlay (decision 58): m=0 is the source, m=1 is 70/30 with magenta, linear in between.
  {
    const float src[3] = {0.2f, 0.6f, 0.4f};
    float o[3];
    overlayPixel(src, 0.0f, o);
    CHECK(o[0] == 0.2f && o[1] == 0.6f && o[2] == 0.4f, "m=0 changed the source");
    overlayPixel(src, 1.0f, o);
    CHECK(std::abs(o[0] - 0.44f) < 1e-6f && std::abs(o[1] - 0.42f) < 1e-6f && std::abs(o[2] - 0.58f) < 1e-6f,
          "m=1 gave %g %g %g", o[0], o[1], o[2]);
    overlayPixel(src, 0.5f, o);
    CHECK(std::abs(o[0] - 0.32f) < 1e-6f && std::abs(o[1] - 0.51f) < 1e-6f && std::abs(o[2] - 0.49f) < 1e-6f,
          "m=0.5 gave %g %g %g", o[0], o[1], o[2]);
  }

  // ACEScg -> display (decision 76), against Flame's aces2.0 config via OCIO (akflame5, 2026-10-27).
  {
    const float cases[3][6] = {{0.18f, 0.18f, 0.18f, 0.4894f, 0.4894f, 0.4894f},
                               {0.2f, 0.1f, 0.05f, 0.5837f, 0.3624f, 0.2614f},
                               {2.0f, 1.0f, 0.5f, 1.0f, 0.9459f, 0.6823f}};  // clamped to 1 (OCIO 1.5235)
    for (const auto& c : cases) {
      float o[3];
      acescgToDisplay(c, o);
      CHECK(std::abs(o[0] - c[3]) < 2e-4f && std::abs(o[1] - c[4]) < 2e-4f && std::abs(o[2] - c[5]) < 2e-4f,
            "acescgToDisplay gave %g %g %g", o[0], o[1], o[2]);
    }
  }

  // Log: format, level filter, correlation ids, rotation.
  char tmpl[] = "/tmp/ltxag_log_test_XXXXXX";
  const std::string home = mkdtemp(tmpl);
  setenv("HOME", home.c_str(), 1);
  setenv("LTXAG_LOG_LEVEL", "info", 1);
  logConfigure();
  {
    LogScope scope("node1234", "job-1");
    LOGI("hello %d", 42);
    LOGD("hidden at info level");
  }
  LOGW("outside scope");
  std::ifstream in(logPath());
  std::string l1, l2, l3;
  std::getline(in, l1);
  std::getline(in, l2);
  CHECK(!std::getline(in, l3), "debug line was written at info level");
  CHECK(l1.find(" INFO  ofx[") == 23 && l1.find("node=node1234 job=job-1 | hello 42") != std::string::npos,
        "bad line: %s", l1.c_str());
  CHECK(l2.find(" WARN  ofx[") == 23 && l2.find("node=- job=- | outside scope") != std::string::npos,
        "bad line: %s", l2.c_str());
  setLogLevel(LogLevel::Debug);
  LOGD("now visible");
  rotateLogs(1, 3);  // anything is over 1 byte: rotate
  LOGI("after rotation");
  struct stat st {};
  CHECK(stat((logPath() + ".1").c_str(), &st) == 0, "no .1 after rotation");
  std::ifstream fresh(logPath());
  std::string f1;
  std::getline(fresh, f1);
  CHECK(f1.find("after rotation") != std::string::npos, "new log not started: %s", f1.c_str());
  for (int i = 0; i < 5; ++i) {
    LOGI("filler %d", i);
    rotateLogs(1, 3);
  }
  CHECK(stat((logPath() + ".3").c_str(), &st) == 0, "rotation did not reach .3");
  CHECK(stat((logPath() + ".4").c_str(), &st) != 0, "kept more than 3 rotated files");
  if (std::system(("rm -rf " + home).c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_marker_log: all checks passed\n");
  return 0;
}
