// Per-run matte index (decision 80): which matte each frame of a Process run produced, so a
// render finds it again even if the source pixels differ slightly from Process time (a denoiser
// upstream that is not bit-identical between renders). Layout under the cache root:
//   <root>/index/<run id>.idx   written by the plugin at Process (atomically), never by job.py
// The matte files and their keys are unchanged (cache.h); the index only records the keys.
// A small thumbnail of every source frame (cell means) lets render tell a changed source apart
// from denoise or grain noise: a changed source only adds a note, it never withholds the matte.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ltxag {

// Thumbnail: kThumbW x kThumbH cells, mean RGB of every kThumbStep-th pixel of every
// kThumbStep-th row (non-finite samples skipped). Changing any of these makes old indexes
// unreadable (they are checked on load), which only means the exact-hash lookup is used.
constexpr int kThumbW = 32, kThumbH = 18, kThumbStep = 4;
constexpr int kThumbSize = kThumbW * kThumbH * 3;

// Source-changed note (tunable; the measured difference is logged per render at DEBUG):
// mean |a - b| over the thumbnail, relative to the Process-time mean |a| plus kChangedFloor.
// Deliberately lenient: grain and denoise variation averages out in the cell means (well under
// 1% at HD); another shot, a grade or a large shift is far above it.
constexpr double kSourceChangedThreshold = 0.05;
constexpr double kChangedFloor = 0.01;  // keeps near-black frames from dividing by ~0

struct MatteIndex {
  int first = 0;                  // OFX time of frame 0
  int width = 0, height = 0;      // source frame size at Process
  std::string sig;                // paramSignature() at Process
  std::vector<std::string> keys;  // matte key per frame (matteKey, cache.h)
  std::vector<float> thumbs;      // kThumbSize floats per frame
  int count() const { return int(keys.size()); }
  const float* thumb(int i) const { return thumbs.data() + size_t(i) * kThumbSize; }
};

inline std::string indexDir(const std::string& root) { return root + "/index"; }
inline std::string indexPath(const std::string& root, const std::string& runId) {
  return indexDir(root) + "/" + runId + ".idx";
}

// `row(y)` gives row y (0 <= y < h, any fixed order) of `comps` (>= 3) floats per pixel.
std::vector<float> sourceThumb(int w, int h, int comps, const std::function<const float*(int)>& row);

// Relative difference of a Process-time thumbnail `ref` and a current one `cur`.
double thumbDifference(const float* ref, const float* cur);

// Writes to a temporary file in the same folder, then renames (a reader never sees half a file).
bool writeMatteIndex(const std::string& path, const MatteIndex& idx, std::string& err);

// False if missing, truncated, another version or thumbnail layout: then there is no index.
// Refreshes the file's mtime on success (the age sweep, as for mattes).
bool readMatteIndex(const std::string& path, MatteIndex& idx);

}  // namespace ltxag
