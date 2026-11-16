// Tests for the disk matte cache and its purge/sweep rules (decision 36).
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "cache.h"
#include "half.h"
#include "platform.h"
#include "transport.h"

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

static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

// Writes a w*h matte file the way worker/ltxag_worker/cache.py does, aged by `ageSec`.
static void writeMatte(const std::string& root, const std::string& key, uint32_t w, uint32_t h,
                       uint16_t value, long ageSec) {
  const std::string p = mattePath(root, key);
  mkdir(p.substr(0, p.rfind('/')).c_str(), 0755);
  std::ofstream out(p, std::ios::binary);
  FrameFileHeader hdr{{'L', 'T', 'X', 'M'}, kTransportVersion, w, h, 1, 1};
  out.write(reinterpret_cast<const char*>(&hdr), sizeof hdr);
  std::vector<uint16_t> px(size_t(w) * h, value);
  out.write(reinterpret_cast<const char*>(px.data()), px.size() * 2);
  out.close();
  const timespec t[2] = {{time(nullptr) - ageSec, 0}, {time(nullptr) - ageSec, 0}};
  utimensat(AT_FDCWD, p.c_str(), t, 0);
}

static int deadPid() {
  const pid_t pid = fork();
  if (pid == 0) _exit(0);
  waitpid(pid, nullptr, 0);
  return pid;  // reaped, so no longer alive
}

int main() {
  char tmpl[] = "/tmp/ltxag_cache_test_XXXXXX";
  const std::string root = mkdtemp(tmpl);

  // Keys: stable, 16 hex chars, sensitive to both source hash and params.
  const std::string k1 = matteKey(42, "v1|cs=Bypass");
  CHECK(k1.size() == 16, "key length %zu", k1.size());
  CHECK(k1 == matteKey(42, "v1|cs=Bypass"), "key not stable");
  CHECK(k1 != matteKey(43, "v1|cs=Bypass"), "key ignores source hash");
  CHECK(k1 != matteKey(42, "v1|cs=ACEScg"), "key ignores params");

  // Read back + mtime refresh.
  writeMatte(root, k1, 4, 3, 0x3c00 /* 1.0 */, 5 * 86400);
  std::vector<uint16_t> m;
  CHECK(readMatte(root, k1, 4, 3, m) && m.size() == 12 && m[0] == 0x3c00, "readMatte");
  struct stat st {};
  stat(mattePath(root, k1).c_str(), &st);
  CHECK(time(nullptr) - st.st_mtime < 60, "read did not refresh mtime");
  CHECK(!readMatte(root, k1, 5, 3, m), "size mismatch must miss");
  CHECK(!readMatte(root, "ffffffffffffffff", 4, 3, m), "missing key must miss");

  // Pending markers: alive pid -> pending; dead pid -> not pending, removed by sweep.
  mkdir((root + "/pending").c_str(), 0755);
  std::ofstream(pendingPath(root, "aaaaaaaaaaaaaaaa")) << getpid();
  std::ofstream(pendingPath(root, "bbbbbbbbbbbbbbbb")) << deadPid();
  CHECK(isPending(root, "aaaaaaaaaaaaaaaa"), "live job not pending");
  CHECK(!isPending(root, "bbbbbbbbbbbbbbbb"), "dead job still pending");
  markPending(root, {"cccccccccccccccc", "dddddddddddddddd"}, getpid());
  CHECK(isPending(root, "cccccccccccccccc") && isPending(root, "dddddddddddddddd"), "markPending");
  clearPending(root, {"cccccccccccccccc", "dddddddddddddddd"});
  CHECK(!isPending(root, "cccccccccccccccc"), "clearPending left a marker");

  // anyJobActive: a live job's marker blocks; a dead one does not; our own stale marker lapses.
  CHECK(anyJobActive(root, getpid()), "live job not active");  // 'aaaa…' holds our pid, fresh
  clearPending(root, {"aaaaaaaaaaaaaaaa"});
  CHECK(!anyJobActive(root, getpid()), "dead marker blocks Process");
  std::ofstream(pendingPath(root, "eeeeeeeeeeeeeeee")) << getppid();  // another live process
  CHECK(anyJobActive(root, getpid()), "other live pid does not block");
  CHECK(anyJobActive(root, getpid(), 0) , "foreign pid must ignore the grace period");
  clearPending(root, {"eeeeeeeeeeeeeeee"});
  markPending(root, {"ffffffffffffffff"}, getpid());
  CHECK(anyJobActive(root, getpid()), "fresh own marker (launch gap) not active");
  struct timespec old[2] = {{time(nullptr) - 600, 0}, {time(nullptr) - 600, 0}};
  utimensat(AT_FDCWD, pendingPath(root, "ffffffffffffffff").c_str(), old, 0);
  CHECK(!anyJobActive(root, getpid()), "stale own marker blocks forever");
  clearPending(root, {"ffffffffffffffff"});
  std::ofstream(pendingPath(root, "aaaaaaaaaaaaaaaa")) << getpid();  // back for the sweep below

  // Sweep: age limit, then LRU down to the cap.
  CacheSettings s;
  s.root = root;
  s.maxAgeDays = 30;
  const uint32_t W = 256, H = 256;  // 128 KiB per matte
  writeMatte(root, "a1aaaaaaaaaaaaaa", W, H, 1, 40 * 86400);  // too old
  writeMatte(root, "b1bbbbbbbbbbbbbb", W, H, 1, 3 * 86400);   // oldest of the rest
  writeMatte(root, "c1cccccccccccccc", W, H, 1, 2 * 86400);
  writeMatte(root, "d1dddddddddddddd", W, H, 1, 1 * 86400);
  s.capGB = 2.5 * (W * H * 2 + sizeof(FrameFileHeader)) / 1073741824.0;  // room for 2 big + k1
  SweepResult r = sweepCache(s);
  CHECK(r.aged == 1 && !exists(mattePath(root, "a1aaaaaaaaaaaaaa")), "aged=%zu", r.aged);
  CHECK(r.capped == 1 && !exists(mattePath(root, "b1bbbbbbbbbbbbbb")), "capped=%zu", r.capped);
  CHECK(exists(mattePath(root, "c1cccccccccccccc")) && exists(mattePath(root, "d1dddddddddddddd")) &&
            exists(mattePath(root, k1)),
        "recent entries must survive");
  CHECK(exists(pendingPath(root, "aaaaaaaaaaaaaaaa")), "live pending removed");
  CHECK(!exists(pendingPath(root, "bbbbbbbbbbbbbbbb")), "dead pending kept");

  // Purges.
  CHECK(purgeKeys(root, {"c1cccccccccccccc", "nonexistent00000"}) == 1, "purgeKeys");
  CHECK(cacheBytes(root) > 0, "cacheBytes");
  CHECK(purgeAll(root) > 0 && cacheBytes(root) == 0, "purgeAll");

  // Staging sweep: dead-owner files go, live-owner and live-job files stay.
  const int dead = deadPid();
  const std::string deadFile = stagingDir() + "/ltxag_" + std::to_string(dead) + "_0_in.bin";
  const std::string liveFile = stagingDir() + "/ltxag_" + std::to_string(getpid()) + "_0_in.bin";
  const std::string jobLive = stagingDir() + "/ltxag_job_t" + std::to_string(getpid()) + "-1";
  const std::string jobDead = stagingDir() + "/ltxag_job_t" + std::to_string(dead) + "-2";
  std::ofstream(deadFile) << "x";
  std::ofstream(liveFile) << "x";
  std::ofstream(jobLive + "_in.bin") << "x";
  std::ofstream(jobLive + ".pid") << getpid();
  std::ofstream(jobDead + "_in.bin") << "x";
  std::ofstream(jobDead + ".pid") << dead;
  sweepStaleStaging();
  CHECK(!exists(deadFile), "dead-owner shm file kept");
  CHECK(exists(liveFile), "live-owner shm file removed");
  CHECK(exists(jobLive + "_in.bin") && exists(jobLive + ".pid"), "live job files removed");
  CHECK(!exists(jobDead + "_in.bin"), "dead job input kept");
  for (const auto& p : {liveFile, jobLive + "_in.bin", jobLive + ".pid", jobDead + ".pid"})
    unlink(p.c_str());

  // Half conversion (half.h, F16C or NEON): exact values, round to nearest even, vector + tail.
  const float fs[] = {0.0f,        1.0f, -2.0f, 0.5f, 65504.0f, 1.0f / 3.0f,
                      1.0f + 3.0f / 2048.0f /* tie */, 0.25f, 3.0f};
  const uint16_t hs[] = {0x0000, 0x3c00, 0xc000, 0x3800, 0x7bff, 0x3555, 0x3c02, 0x3400, 0x4200};
  uint16_t hOut[9];
  float fOut[9];
  floatToHalf(fs, hOut, 9);
  halfToFloat(hs, fOut, 9);
  for (int i = 0; i < 9; ++i) {
    CHECK(hOut[i] == hs[i], "floatToHalf[%d] = 0x%04x", i, hOut[i]);
    CHECK(i == 5 || i == 6 || fOut[i] == fs[i], "halfToFloat[%d] = %g", i, fOut[i]);
  }

  // Frame file space is reserved at open (posix_fallocate / F_PREALLOCATE + ftruncate).
  {
    StagingFile sf("t");
    FrameWriter fw;
    std::string err;
    CHECK(fw.open(sf.path(), 4, 2, 3, err), "FrameWriter open: %s", err.c_str());
    struct stat fst {};
    CHECK(stat(sf.path().c_str(), &fst) == 0 &&
              fst.st_size == off_t(sizeof(FrameFileHeader) + 4 * 2 * 3 * 2 * 3),
          "staging file size %lld", (long long)fst.st_size);
    fw.close();
  }
#if defined(__APPLE__)
  {
    StagingFile sf("big");  // ~1.5 PB: must fail at open, not while writing frames
    FrameWriter fw;
    std::string err;
    CHECK(!fw.open(sf.path(), 65535, 65535, 65535, err), "huge reservation succeeded");
    fw.close();
  }
#endif

  if (std::system(("rm -rf " + root).c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_cache: all checks passed\n");
  return 0;
}
