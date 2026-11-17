// Tests for the per-run matte index (decision 80): file round trip, atomic replace, rejected
// files, thumbnail determinism and the forgiving source comparison.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "matte_index.h"

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

// A textured RGBA frame with a disc at (cx, cy): stands in for a shot.
static std::vector<float> frame(int w, int h, double cx, double cy, unsigned seed, float noise) {
  std::vector<float> px(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      seed = seed * 1103515245u + 12345u;
      const float n = noise * (float((seed >> 8) & 0xffff) / 65535.0f - 0.5f) * 2.0f;
      const bool disc = std::hypot(x - cx, y - cy) < 0.2 * h;
      float* p = &px[(size_t(y) * w + x) * 4];
      p[0] = (disc ? 0.85f : 0.25f + 0.2f * std::sin(x / 37.0f)) + n;
      p[1] = (disc ? 0.55f : 0.35f + 0.2f * std::cos(y / 53.0f)) + n;
      p[2] = (disc ? 0.40f : 0.30f) + n;
      p[3] = 1.0f;
    }
  return px;
}

static std::vector<float> thumb(const std::vector<float>& px, int w, int h) {
  return sourceThumb(w, h, 4, [&](int y) { return &px[size_t(y) * w * 4]; });
}

static int filesIn(const std::string& dir) {
  int n = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) n += e->d_name[0] != '.';
    closedir(d);
  }
  return n;
}

int main() {
  char tmpl[] = "/tmp/ltxag_index_test_XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const int W = 640, H = 360;

  // Thumbnail: deterministic, the right size, finite even with NaN/inf pixels.
  const auto a = frame(W, H, 200, 180, 1, 0.0f);
  const auto ta = thumb(a, W, H);
  CHECK(ta.size() == size_t(kThumbSize), "thumb size %zu", ta.size());
  CHECK(thumb(a, W, H) == ta, "thumb not deterministic");
  CHECK(thumbDifference(ta.data(), ta.data()) == 0.0, "identical thumbs differ");
  auto bad = a;
  bad[0] = NAN;
  bad[4] = INFINITY;
  const auto tb = thumb(bad, W, H);
  bool finite = true;
  for (float v : tb) finite = finite && std::isfinite(v);
  CHECK(finite, "non-finite pixels leak into the thumbnail");
  CHECK(thumbDifference(ta.data(), tb.data()) < 0.01, "two bad pixels change the thumbnail");
  // Tiny frames (fewer samples than cells) still work.
  const auto tiny = frame(20, 10, 10, 5, 1, 0.0f);
  CHECK(thumb(tiny, 20, 10).size() == size_t(kThumbSize), "tiny frame");

  // Forgiving comparison: grain/denoise-level noise is clean, another picture is changed.
  const double grain = thumbDifference(ta.data(), thumb(frame(W, H, 200, 180, 7, 0.05f), W, H).data());
  const double heavy = thumbDifference(ta.data(), thumb(frame(W, H, 200, 180, 9, 0.15f), W, H).data());
  const double moved = thumbDifference(ta.data(), thumb(frame(W, H, 440, 180, 1, 0.0f), W, H).data());
  auto inv = a;
  for (size_t i = 0; i < inv.size(); ++i)
    if (i % 4 != 3) inv[i] = 1.0f - inv[i];
  const double other = thumbDifference(ta.data(), thumb(inv, W, H).data());
  auto graded = a;
  for (size_t i = 0; i < graded.size(); ++i)
    if (i % 4 != 3) graded[i] *= 1.2f;
  const double grade = thumbDifference(ta.data(), thumb(graded, W, H).data());
  std::printf("difference: grain 5%% %.4f, grain 15%% %.4f, disc moved %.4f, inverted %.4f, +20%% gain %.4f "
              "(threshold %.2f)\n", grain, heavy, moved, other, grade, kSourceChangedThreshold);
  CHECK(grain < kSourceChangedThreshold, "grain flagged: %.4f", grain);
  CHECK(heavy < kSourceChangedThreshold, "heavy grain flagged: %.4f", heavy);
  CHECK(moved > kSourceChangedThreshold, "moved subject not flagged: %.4f", moved);
  CHECK(other > kSourceChangedThreshold, "another picture not flagged: %.4f", other);
  CHECK(grade > kSourceChangedThreshold, "+20%% gain not flagged: %.4f", grade);
  // Near-black frames: the floor keeps noise from looking like a change.
  std::vector<float> dark(size_t(W) * H * 4, 0.001f), dark2 = dark;
  for (size_t i = 0; i < dark2.size(); i += 7) dark2[i] = 0.003f;
  CHECK(thumbDifference(thumb(dark, W, H).data(), thumb(dark2, W, H).data()) < kSourceChangedThreshold,
        "noise on black flagged");

  // Round trip.
  MatteIndex idx;
  idx.first = 0, idx.width = W, idx.height = H, idx.sig = "v1|cs=Bypass|prec=fp8|model=dummy-0.3";
  for (int f = 0; f < 3; ++f) {
    idx.keys.push_back(std::string(15, 'a') + char('0' + f));
    const auto t = thumb(frame(W, H, 100 + 50 * f, 180, 1, 0.0f), W, H);
    idx.thumbs.insert(idx.thumbs.end(), t.begin(), t.end());
  }
  const std::string path = indexPath(root, "123-0-1700000000");
  std::string err;
  CHECK(writeMatteIndex(path, idx, err), "write: %s", err.c_str());
  MatteIndex back;
  CHECK(readMatteIndex(path, back), "read back");
  CHECK(back.first == 0 && back.width == W && back.height == H && back.sig == idx.sig && back.keys == idx.keys &&
            back.thumbs == idx.thumbs,
        "round trip differs");
  CHECK(filesIn(indexDir(root)) == 1, "temporary file left: %d files", filesIn(indexDir(root)));

  // Atomic replace: a rewrite of the same path gives the new content, no temporary left.
  idx.first = 5;
  idx.keys[0] = "bbbbbbbbbbbbbbbb";
  CHECK(writeMatteIndex(path, idx, err), "rewrite: %s", err.c_str());
  CHECK(readMatteIndex(path, back) && back.first == 5 && back.keys[0] == "bbbbbbbbbbbbbbbb", "rewrite read");
  CHECK(filesIn(indexDir(root)) == 1, "temporary file left after rewrite");
  // Inconsistent input is refused, the old file stays.
  MatteIndex wrong = idx;
  wrong.thumbs.pop_back();
  CHECK(!writeMatteIndex(path, wrong, err), "thumbs/keys mismatch accepted");
  wrong = idx;
  wrong.keys[1] = "short";
  CHECK(!writeMatteIndex(path, wrong, err), "short key accepted");
  CHECK(readMatteIndex(path, back) && back.first == 5, "refused write damaged the index");

  // Missing, truncated, corrupt or another version: no index (and `back` untouched).
  MatteIndex none;
  CHECK(!readMatteIndex(indexPath(root, "missing"), none), "missing file read");
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto writeBytes = [&](const std::string& p, const std::string& b) {
    std::ofstream(p, std::ios::binary | std::ios::trunc).write(b.data(), std::streamsize(b.size()));
  };
  const std::string cut = indexPath(root, "cut");
  writeBytes(cut, bytes.substr(0, bytes.size() - 5));
  CHECK(!readMatteIndex(cut, none), "truncated file read");
  writeBytes(cut, bytes + "x");
  CHECK(!readMatteIndex(cut, none), "file with trailing bytes read");
  std::string magic = bytes;
  magic[0] = 'X';
  writeBytes(cut, magic);
  CHECK(!readMatteIndex(cut, none), "wrong magic read");
  std::string ver = bytes;
  ver[4] = char(ver[4] + 1);  // version (uint32 after the magic)
  writeBytes(cut, ver);
  CHECK(!readMatteIndex(cut, none), "another version read");
  writeBytes(cut, "");
  CHECK(!readMatteIndex(cut, none), "empty file read");
  CHECK(none.keys.empty(), "a failed read filled the index");

  (void)!std::system(("rm -rf '" + root + "'").c_str());
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  else std::printf("matte index: all checks passed\n");
  return failures ? 1 : 0;
}
