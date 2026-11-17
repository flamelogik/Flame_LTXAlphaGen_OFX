#include "matte_index.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ltxag {

namespace {

constexpr char kMagic[4] = {'L', 'T', 'X', 'I'};
constexpr uint32_t kVersion = 1;
constexpr int kKeyLen = 16;  // matteKey(): 16 hex digits

// Fixed-size header; the param signature follows it, then the keys, then the thumbnails.
struct Header {
  char magic[4];
  uint32_t version;
  int32_t first, count, width, height, thumbW, thumbH, thumbStep;
  uint32_t sigLen;
};

}  // namespace

std::vector<float> sourceThumb(int w, int h, int comps, const std::function<const float*(int)>& row) {
  std::vector<double> sum(kThumbSize, 0.0);
  std::vector<int> n(kThumbW * kThumbH, 0);
  for (int y = 0; y < h; y += kThumbStep) {
    const float* p = row(y);
    const int cy = int(int64_t(y) * kThumbH / h);
    for (int x = 0; x < w; x += kThumbStep) {
      const float* px = p + size_t(x) * comps;
      if (!std::isfinite(px[0]) || !std::isfinite(px[1]) || !std::isfinite(px[2])) continue;
      const int c = cy * kThumbW + int(int64_t(x) * kThumbW / w);
      for (int k = 0; k < 3; ++k) sum[size_t(c) * 3 + k] += px[k];
      ++n[c];
    }
  }
  std::vector<float> out(kThumbSize, 0.0f);
  for (int c = 0; c < kThumbW * kThumbH; ++c)
    if (n[c])
      for (int k = 0; k < 3; ++k) out[size_t(c) * 3 + k] = float(sum[size_t(c) * 3 + k] / n[c]);
  return out;
}

double thumbDifference(const float* ref, const float* cur) {
  double d = 0.0, m = 0.0;
  for (int i = 0; i < kThumbSize; ++i) {
    d += std::fabs(double(ref[i]) - cur[i]);
    m += std::fabs(double(ref[i]));
  }
  return (d / kThumbSize) / (m / kThumbSize + kChangedFloor);
}

bool writeMatteIndex(const std::string& path, const MatteIndex& idx, std::string& err) {
  if (idx.thumbs.size() != size_t(idx.count()) * kThumbSize) {
    err = "thumbnail count does not match the frame count";
    return false;
  }
  for (const auto& k : idx.keys)
    if (k.size() != size_t(kKeyLen)) {
      err = "unexpected matte key length";
      return false;
    }
  mkdir(path.substr(0, path.rfind('/')).c_str(), 0755);
  const std::string tmp = path + ".tmp" + std::to_string(getpid());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    Header hd{{kMagic[0], kMagic[1], kMagic[2], kMagic[3]}, kVersion, idx.first, idx.count(), idx.width,
              idx.height, kThumbW, kThumbH, kThumbStep, uint32_t(idx.sig.size())};
    out.write(reinterpret_cast<const char*>(&hd), sizeof hd);
    out.write(idx.sig.data(), std::streamsize(idx.sig.size()));
    for (const auto& k : idx.keys) out.write(k.data(), kKeyLen);
    out.write(reinterpret_cast<const char*>(idx.thumbs.data()), std::streamsize(idx.thumbs.size() * sizeof(float)));
    out.close();
    if (!out) {
      err = "could not write " + tmp + ": " + std::strerror(errno);
      unlink(tmp.c_str());
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "could not rename " + tmp + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool readMatteIndex(const std::string& path, MatteIndex& idx) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0);
  Header hd{};
  if (!in.read(reinterpret_cast<char*>(&hd), sizeof hd) || std::memcmp(hd.magic, kMagic, 4) != 0 ||
      hd.version != kVersion || hd.thumbW != kThumbW || hd.thumbH != kThumbH || hd.thumbStep != kThumbStep ||
      hd.count <= 0 || hd.count > 1000000 || hd.sigLen > 4096)
    return false;
  const std::streamoff want = std::streamoff(sizeof hd) + hd.sigLen +
                              std::streamoff(hd.count) * (kKeyLen + kThumbSize * std::streamoff(sizeof(float)));
  if (size != want) return false;
  MatteIndex r;
  r.first = hd.first, r.width = hd.width, r.height = hd.height;
  r.sig.resize(hd.sigLen);
  in.read(&r.sig[0], hd.sigLen);
  std::vector<char> keys(size_t(hd.count) * kKeyLen);
  in.read(keys.data(), std::streamsize(keys.size()));
  r.keys.reserve(size_t(hd.count));
  for (int i = 0; i < hd.count; ++i) r.keys.emplace_back(&keys[size_t(i) * kKeyLen], kKeyLen);
  r.thumbs.resize(size_t(hd.count) * kThumbSize);
  in.read(reinterpret_cast<char*>(r.thumbs.data()), std::streamsize(r.thumbs.size() * sizeof(float)));
  if (!in) return false;
  idx = std::move(r);
  utimensat(AT_FDCWD, path.c_str(), nullptr, 0);  // recently used: kept by the age sweep
  return true;
}

}  // namespace ltxag
