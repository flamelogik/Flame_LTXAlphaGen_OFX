// Content-addressed per-frame matte cache on disk (decision 36).
// Layout under the cache root (must match worker/ltxag_worker/cache.py):
//   <root>/<k[0:2]>/<key>.matte   one matte: FrameFileHeader "LTXM", n=1, then w*h fp16
//   <root>/pending/<key>          exists while a job is computing that key; holds the job pid
//   <root>/jobs/<job id>.json     final job status (done / failed / cancelled + message)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "user_config.h"

namespace ltxag {

struct CacheSettings {
  std::string root;
  int maxAgeDays = 30;
  double capGB = 50.0;
};

CacheSettings cacheSettings(const UserConfig& cfg);

// Everything that changes the matte besides the source pixels.
std::string matteKey(uint64_t sourceFrameHash, const std::string& paramSignature);

std::string mattePath(const std::string& root, const std::string& key);
std::string pendingPath(const std::string& root, const std::string& key);

// Reads one matte (w*h halves, rows top-down) and refreshes its mtime for the LRU sweep.
bool readMatte(const std::string& root, const std::string& key, uint32_t w, uint32_t h,
               std::vector<uint16_t>& out);

// True while a live job holds the key's pending marker.
bool isPending(const std::string& root, const std::string& key);

// True while any job (this node or another) is live: a pending marker whose pid is alive.
// Markers the plugin wrote with its own pid (`selfPid`) count only for `graceSec` seconds, so a
// job.py that never started cannot block Process until Flame restarts.
bool anyJobActive(const std::string& root, int selfPid, int graceSec = 120);

// Written by the plugin just before it launches a job (with Flame's pid), so a render that
// arrives before job.py has marked the keys still shows PROCESSING; job.py then rewrites them
// with its own pid and removes them at the end. clearPending undoes it if the launch fails.
void markPending(const std::string& root, const std::vector<std::string>& keys, int pid);
void clearPending(const std::string& root, const std::vector<std::string>& keys);

size_t purgeKeys(const std::string& root, const std::vector<std::string>& keys);
uint64_t cacheBytes(const std::string& root);
uint64_t purgeAll(const std::string& root);  // returns bytes freed

struct SweepResult {
  size_t aged = 0, capped = 0, kept = 0;
  uint64_t bytesFreed = 0, bytesKept = 0;
};
// Deletes mattes unused for maxAgeDays, then least-recently-used until under capGB.
// Also clears pending markers of dead jobs.
SweepResult sweepCache(const CacheSettings& s);

// Removes stagingDir()/ltxag_* files whose owner is gone (test 1 left a 1.5 GB orphan).
size_t sweepStaleStaging();

bool pidAlive(int pid);

}  // namespace ltxag
