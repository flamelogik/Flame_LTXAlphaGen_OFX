#include "cache.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#include "matte_index.h"
#include "platform.h"
#include "transport.h"

namespace ltxag {

namespace {

std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] != '.') out.emplace_back(e->d_name);
    }
    closedir(d);
  }
  return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int readPid(const std::string& path) {
  std::ifstream in(path);
  int pid = 0;
  in >> pid;
  return pid;
}

}  // namespace

bool pidAlive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

CacheSettings cacheSettings(const UserConfig& cfg) {
  CacheSettings s;
  const char* home = std::getenv("HOME");
  s.root = cfg.count("cache_dir") ? cfg.at("cache_dir")
                                  : std::string(home ? home : "/tmp") + "/.cache/ltx-alphagen/mattes";
  if (cfg.count("cache_max_days")) s.maxAgeDays = std::atoi(cfg.at("cache_max_days").c_str());
  if (cfg.count("cache_cap_gb")) s.capGB = std::atof(cfg.at("cache_cap_gb").c_str());
  return s;
}

std::string matteKey(uint64_t sourceFrameHash, const std::string& paramSignature) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < 8; ++i) h = (h ^ ((sourceFrameHash >> (8 * i)) & 0xff)) * 1099511628211ULL;
  for (unsigned char c : paramSignature) h = (h ^ c) * 1099511628211ULL;
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

std::string mattePath(const std::string& root, const std::string& key) {
  return root + "/" + key.substr(0, 2) + "/" + key + ".matte";
}

std::string pendingPath(const std::string& root, const std::string& key) {
  return root + "/pending/" + key;
}

bool readMatte(const std::string& root, const std::string& key, uint32_t w, uint32_t h,
               std::vector<uint16_t>& out) {
  const std::string path = mattePath(root, key);
  std::string err;
  if (!readMattes(path, w, h, 1, out, err)) return false;
  utimensat(AT_FDCWD, path.c_str(), nullptr, 0);  // mark as recently used
  return true;
}

bool isPending(const std::string& root, const std::string& key) {
  const std::string p = pendingPath(root, key);
  if (access(p.c_str(), F_OK) != 0) return false;
  return pidAlive(readPid(p));
}

void markPending(const std::string& root, const std::vector<std::string>& keys, int pid) {
  mkdir((root + "/pending").c_str(), 0755);
  const std::string text = std::to_string(pid);
  for (const auto& k : keys) {
    const int fd = open(pendingPath(root, k).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) continue;
    (void)!write(fd, text.data(), text.size());
    close(fd);
  }
}

void clearPending(const std::string& root, const std::vector<std::string>& keys) {
  for (const auto& k : keys) unlink(pendingPath(root, k).c_str());
}

size_t purgeKeys(const std::string& root, const std::vector<std::string>& keys) {
  size_t n = 0;
  for (const auto& k : keys)
    if (unlink(mattePath(root, k).c_str()) == 0) ++n;
  return n;
}

uint64_t cacheBytes(const std::string& root) {
  uint64_t total = 0;
  for (const auto& sub : listDir(root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(root + "/" + sub)) {
      struct stat st {};
      if (endsWith(f, ".matte") && stat((root + "/" + sub + "/" + f).c_str(), &st) == 0)
        total += uint64_t(st.st_size);
    }
  }
  return total;
}

uint64_t purgeAll(const std::string& root) {
  uint64_t freed = 0;
  for (const auto& sub : listDir(root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(root + "/" + sub)) {
      const std::string p = root + "/" + sub + "/" + f;
      struct stat st {};
      if (endsWith(f, ".matte") && stat(p.c_str(), &st) == 0 && unlink(p.c_str()) == 0)
        freed += uint64_t(st.st_size);
    }
  }
  for (const auto& f : listDir(indexDir(root))) {  // run indexes (decision 80)
    const std::string p = indexDir(root) + "/" + f;
    struct stat st {};
    if (stat(p.c_str(), &st) == 0 && unlink(p.c_str()) == 0) freed += uint64_t(st.st_size);
  }
  return freed;
}

SweepResult sweepCache(const CacheSettings& s) {
  SweepResult r;
  struct Entry {
    std::string path;
    time_t mtime;
    uint64_t size;
  };
  std::vector<Entry> entries;
  const time_t now = time(nullptr);
  const time_t maxAge = time_t(s.maxAgeDays) * 86400;
  for (const auto& sub : listDir(s.root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(s.root + "/" + sub)) {
      if (!endsWith(f, ".matte")) continue;
      const std::string p = s.root + "/" + sub + "/" + f;
      struct stat st {};
      if (stat(p.c_str(), &st) != 0) continue;
      if (s.maxAgeDays > 0 && now - st.st_mtime > maxAge) {
        if (unlink(p.c_str()) == 0) {
          ++r.aged;
          r.bytesFreed += uint64_t(st.st_size);
        }
        continue;
      }
      entries.push_back({p, st.st_mtime, uint64_t(st.st_size)});
    }
  }
  uint64_t total = 0;
  for (const auto& e : entries) total += e.size;
  const uint64_t cap = uint64_t(s.capGB * 1024.0 * 1024.0 * 1024.0);
  if (s.capGB > 0 && total > cap) {
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });  // oldest first
    for (const auto& e : entries) {
      if (total <= cap) break;
      if (unlink(e.path.c_str()) == 0) {
        total -= e.size;
        ++r.capped;
        r.bytesFreed += e.size;
      }
    }
  }
  r.bytesKept = total;
  r.kept = entries.size() - r.capped;
  // Run indexes (decision 80) unused for maxAgeDays; small, so not part of the size cap.
  for (const auto& f : listDir(indexDir(s.root))) {
    const std::string p = indexDir(s.root) + "/" + f;
    struct stat st {};
    if (s.maxAgeDays > 0 && stat(p.c_str(), &st) == 0 && now - st.st_mtime > maxAge && unlink(p.c_str()) == 0)
      ++r.indexesAged;
  }
  // Pending markers of jobs that no longer run.
  for (const auto& f : listDir(s.root + "/pending")) {
    const std::string p = s.root + "/pending/" + f;
    if (!pidAlive(readPid(p))) unlink(p.c_str());
  }
  return r;
}

size_t sweepStaleStaging() {
  // ltxag_<flame pid>_<seq>_*.bin : transient files of a Flame process -> owner must be alive.
  // ltxag_job_<id>[_...|.ext]     : handed to a job (<id> has no '_' or '.');
  //                                 ltxag_job_<id>.pid names the job's pid.
  size_t removed = 0;
  const std::string dir = stagingDir();
  const auto files = listDir(dir);
  const time_t now = time(nullptr);
  const std::string jobPrefix = "ltxag_job_";
  auto jobBase = [&](const std::string& f) {
    return f.substr(0, f.find_first_of("_.", jobPrefix.size()));
  };
  // Decide every job's fate before deleting anything: removing a .pid file first
  // would make its sibling files look like a job that has not started yet.
  std::vector<std::string> deadJobs, liveJobs;
  for (const auto& f : files)
    if (f.rfind(jobPrefix, 0) == 0 && endsWith(f, ".pid"))
      (pidAlive(readPid(dir + "/" + f)) ? liveJobs : deadJobs).push_back(jobBase(f));
  auto contains = [](const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  };
  for (const auto& f : files) {
    if (f.rfind("ltxag_", 0) != 0) continue;
    const std::string p = dir + "/" + f;
    bool stale = false;
    if (f.rfind(jobPrefix, 0) == 0) {
      const std::string base = jobBase(f);
      struct stat st {};
      const bool young = stat(p.c_str(), &st) == 0 && now - st.st_mtime < 600;
      // No pid file yet: a job being launched (young) or one that never started (old).
      stale = contains(deadJobs, base) || (!contains(liveJobs, base) && !young);
    } else {
      stale = !pidAlive(std::atoi(f.c_str() + 6));
    }
    if (stale && unlink(p.c_str()) == 0) ++removed;
  }
  return removed;
}

}  // namespace ltxag
