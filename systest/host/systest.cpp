// End-to-end system test of the plugin bundle inside a Flame-like OFX host (ofx_host.h).
// See systest/README.md. Two tiers:
//   fast: a dev build with the dummy worker (no GPU work): gate, markers, Process, chunking,
//         merge, ROI, Cancel, failures, memory warning, defaults, purge, logs.
//   full: the release build with the real model: gate as shipped, one real matte checked against
//         the synthetic clip's truth, and a Cancel during denoising.
// Usage: ltxag_systest --bundle <LTXAlphaGen.ofx.bundle> --tier fast|full --out <dir>
//                      --ltx-python <path> [--model-dir <dir>] [--job-python <path>]
//                      [--job-timeout <s>] [--only <test>[,<test>...]]
// It runs inside the HOME prepared by systest/run_systest.sh (isolated config, cache and logs).
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ofx_host.h"
#include "platform.h"

using namespace systest;

namespace {

// ---------------------------------------------------------------- options, results

struct Options {
  std::string bundle, tier = "fast", out = ".", ltxPython, modelDir, jobPython = "/usr/bin/python3";
  std::string fp16;  // the real setup's transformer_fp16 ("" = not set; decision 68)
  double jobTimeout = 0;  // 0: tier default
  std::set<std::string> only;
};

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Skip : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Result {
  std::string name;
  double seconds = 0;
  std::vector<std::string> failures;
  std::string skipped;
  std::vector<std::string> notes;
};

Result* gCur = nullptr;

void note(const std::string& s) {
  std::printf("    . %s\n", s.c_str());
  std::fflush(stdout);
  gCur->notes.push_back(s);
}
void fail(const std::string& s) {
  std::printf("    ! FAIL %s\n", s.c_str());
  std::fflush(stdout);
  gCur->failures.push_back(s);
}
#define CHECK(cond, msg)                                                         \
  do {                                                                           \
    if (!(cond)) fail(std::string(msg) + "  [" #cond "] line " + std::to_string(__LINE__)); \
  } while (0)
#define REQUIRE(cond, msg)                                                       \
  do {                                                                           \
    if (!(cond)) throw Failure(std::string(msg) + "  [" #cond "] line " + std::to_string(__LINE__)); \
  } while (0)

bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

std::string home() { return std::getenv("HOME") ? std::getenv("HOME") : ""; }
std::string fakeUiDir() { return std::getenv("LTXAG_FAKE_UI_DIR") ? std::getenv("LTXAG_FAKE_UI_DIR") : ""; }

std::string readFile(const std::string& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool exists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}
std::vector<std::string> listDir(const std::string& d) {
  std::vector<std::string> v;
  if (DIR* dir = opendir(d.c_str())) {
    while (dirent* e = readdir(dir))
      if (e->d_name[0] != '.') v.push_back(e->d_name);
    closedir(dir);
  }
  std::sort(v.begin(), v.end());
  return v;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Value of "key" in JSON written by Python's json.dump (same reader as the plugin's jsonField).
std::string jsonField(const std::string& j, const std::string& key) {
  const std::string k = "\"" + key + "\": ";
  const auto p = j.find(k);
  if (p == std::string::npos) return "";
  const size_t b = p + k.size();
  if (b < j.size() && j[b] == '"') return j.substr(b + 1, j.find('"', b + 1) - b - 1);
  const auto e = j.find_first_of(",}", b);
  return j.substr(b, e - b);
}

// ---------------------------------------------------------------- the user config

std::string configPath() { return home() + "/.config/ltx-alphagen/config.conf"; }
std::map<std::string, std::string> readConfig() {
  std::map<std::string, std::string> c;
  std::istringstream in(readFile(configPath()));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq != std::string::npos) c[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return c;
}
void writeConfig(const std::map<std::string, std::string>& c) {
  const std::string dir = home() + "/.config/ltx-alphagen";
  mkdir((home() + "/.config").c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  std::ofstream out(configPath(), std::ios::trunc);
  out << "# written by ltxag_systest\n";
  for (const auto& kv : c) out << kv.first << '=' << kv.second << '\n';
}

// ---------------------------------------------------------------- the synthetic clip

// A soft disc with a fuzzy fringe moving over texture (as tools/worker_smoke.py), so the real
// model has a plausible foreground and the truth is known.
enum class AlphaMode { Opaque, Roi, Empty };

struct Scene {
  int w, h, n;
  AlphaMode alpha;
  double size = 1.0;  // < 1: a smaller subject that moves less (leaves room for an ROI crop)
  double jump = 0.0;  // > 0: in the second half the disc jumps this many px back and forth per frame
  // Disc of frame f: centre and radius (pixels, row 0 = top).
  void disc(int f, double& cx, double& cy, double& r) const {
    const double t = double(f) / std::max(1, n - 1);
    cx = w * (0.5 + size * 0.5 * (t - 0.5));
    cy = h * (0.5 + size * 0.15 * std::sin(6.28 * t));
    r = size * 0.22 * std::min(w, h);
    if (jump > 0 && f >= n / 2) cx += (f % 2 ? 0.5 : -0.5) * jump;
  }
  float truth(int f, int x, int y) const {
    double cx, cy, r;
    disc(f, cx, cy, r);
    const double d = std::hypot(x - cx, y - cy);
    const double fringe = 0.5 + 0.5 * std::sin(std::atan2(y - cy, x - cx) * 40.0);
    return float(std::min(1.0, std::max(0.0, (r + 0.06 * r * fringe - d) / (0.02 * r))));
  }
  float roiAlpha(int f, int x, int y) const {  // a soft garbage matte, 1.4x the disc
    double cx, cy, r;
    disc(f, cx, cy, r);
    const double d = std::hypot(x - cx, y - cy);
    return float(std::min(1.0, std::max(0.0, (1.4 * r - d) / (0.1 * r))));
  }
  void frame(int f, std::vector<float>& px) const {
    unsigned seed = 12345;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        seed = seed * 1103515245u + 12345u;
        const float noise = float((seed >> 8) & 0xffff) / 65535.0f;
        const float a = truth(f, x, y);
        float rgb[3] = {0.25f + 0.2f * std::sin(x / 37.0f) + 0.1f * noise, 0.35f + 0.2f * std::cos(y / 53.0f),
                        0.30f + 0.1f * noise};
        const float fg[3] = {0.85f, 0.55f, 0.40f};
        float* p = &px[(size_t(y) * w + x) * 4];
        for (int k = 0; k < 3; ++k) p[k] = rgb[k] * (1 - a) + fg[k] * a;
        p[3] = alpha == AlphaMode::Opaque ? 1.0f : alpha == AlphaMode::Empty ? 0.0f : roiAlpha(f, x, y);
      }
  }
  FrameSource source() const {
    Scene s = *this;
    return [s](int f, int, int, std::vector<float>& px) { s.frame(f, px); };
  }
};

float luma(const float* p) {
  return std::min(1.0f, std::max(0.0f, p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f));
}

// ---------------------------------------------------------------- plugin-facing helpers

std::string cacheRoot(Instance& in) { return in.getString("cacheDir"); }

struct Job {
  std::string id, state, message, json;
  double seconds = 0;
};

std::set<std::string> jobFiles(Instance& in) {
  std::set<std::string> s;
  for (const auto& f : listDir(cacheRoot(in) + "/jobs")) s.insert(f);
  return s;
}

void clearFakeUi() {
  const std::string d = fakeUiDir();
  for (const char* f : {"progress.log", "windows.log", "messages.log", "cancel"}) unlink((d + "/" + f).c_str());
}
std::string fakeProgress() { return readFile(fakeUiDir() + "/progress.log"); }
void requestCancel() { std::ofstream(fakeUiDir() + "/cancel") << "1\n"; }

// macOS: job.py reports a failure with an osascript dialog we can't fake (absolute path). Close
// ours; "[f]" keeps the pattern from matching pkill's own command line.
void closeMacDialogs() {
#ifdef __APPLE__
  sleepMs(1500);
  if (std::system("/usr/bin/pkill -f 'Processing [f]ailed:' >/dev/null 2>&1") == 0)
    note("closed job.py's failure dialog (osascript)");
#endif
}

// Presses Process, finds the job it launched and waits for it to end. `during` runs once per
// poll while the job is running (e.g. to press Cancel). Returns the final status.
Job runJob(Instance& in, double timeout, const std::function<void(Job&)>& during = nullptr) {
  const auto before = jobFiles(in);
  const size_t msgs = in.messages.size();
  clearFakeUi();
  in.press("process");
  Job j;
  const double t0 = now();
  while (now() - t0 < 20 && j.id.empty()) {
    for (const auto& f : jobFiles(in))
      if (!before.count(f)) j.id = f.substr(0, f.size() - 5);
    if (j.id.empty()) {
      bool refused = false;  // an error message and no job (a Yes/No question is not a refusal)
      for (size_t i = msgs; i < in.messages.size(); ++i) refused |= in.messages[i].type == kOfxMessageError;
      if (refused) break;
      sleepMs(100);
    }
  }
  if (j.id.empty()) {
    j.state = "none";
    for (size_t i = msgs; i < in.messages.size(); ++i) j.message += in.messages[i].text;
    return j;
  }
  const std::string path = cacheRoot(in) + "/jobs/" + j.id + ".json";
  while (true) {
    in.pumpUi();  // like a user moving the mouse: Flame applies the watcher's changes now
    j.json = readFile(path);
    j.state = jsonField(j.json, "state");
    j.message = jsonField(j.json, "message");
    j.seconds = now() - t0;
    if (!j.state.empty() && j.state != "running") break;
    if (j.seconds > timeout) {
      j.state = "timeout";
      break;
    }
    if (during) during(j);
    sleepMs(250);
  }
  sleepMs(1200);  // the plugin's watcher polls every 0.5 s
  in.pumpUi();
  return j;
}

std::vector<std::string> stagingLeftovers() {
  std::vector<std::string> v;
  const std::string mine = "ltxag_job_" + std::to_string(getpid()) + "-";
  for (const auto& f : listDir(ltxag::stagingDir()))
    if (startsWith(f, mine)) v.push_back(f);
  return v;
}

void checkClean(Instance& in) {
  const auto left = stagingLeftovers();
  CHECK(left.empty(), "staging files left: " + (left.empty() ? std::string() : left[0]));
  const auto pend = listDir(cacheRoot(in) + "/pending");
  CHECK(pend.empty(), std::to_string(pend.size()) + " pending marker(s) left");
}

// The alpha of a render is a real matte, the marker, or empty.
struct AlphaStats {
  float min = 1e9f, max = -1e9f;
  double mean = 0;
};
AlphaStats alphaStats(const Rendered& r) {
  AlphaStats s;
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      const float a = r.px(x, y)[3];
      s.min = std::min(s.min, a);
      s.max = std::max(s.max, a);
      s.mean += a;
    }
  s.mean /= double(r.w) * r.h;
  return s;
}

// Marker: faint stripes (0.08) and a caption (0.30), decision 44.
void checkMarker(const Rendered& r, const std::string& what) {
  const AlphaStats s = alphaStats(r);
  CHECK(r.status == kOfxStatOK, what + ": render status " + std::to_string(r.status));
  CHECK(s.min >= 0.0f && s.max > 0.05f && s.max < 0.5f && s.mean < 0.2,
        what + ": alpha is not the marker (min " + std::to_string(s.min) + " max " + std::to_string(s.max) +
            " mean " + std::to_string(s.mean) + ")");
}

// Decision 71: a processed node without a matte fails the render, after filling both outputs
// with the marker (grey on black, as SETUP REQUIRED).
void checkFailed(const Rendered& r, const std::string& what) {
  CHECK(r.status == kOfxStatFailed, what + ": render status " + std::to_string(r.status) + ", expected failed");
  const AlphaStats s = alphaStats(r);
  CHECK(s.min >= 0.0f && s.max > 0.05f && s.max < 0.5f && s.mean < 0.2, what + ": alpha is not the marker");
  bool grey = true;
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      const float* p = r.px(x, y);
      if (p[0] != p[3] || p[1] != p[3] || p[2] != p[3]) grey = false;
    }
  CHECK(grey, what + ": image output = the marker");
}

// RGB = the source with the matte overlaid in magenta at 30% (decision 58).
float overlay(float src, float m, int k) { return src * (1.0f - 0.3f * m) + (k == 1 ? 0.0f : 0.3f * m); }

// Compares a render with the expected matte (alpha) and overlay (RGB). `expect(x, y)` gives the
// matte; returns the largest alpha error.
float checkMatte(const Rendered& r, const Scene& sc, int f, const std::function<float(const float*, int, int)>& expect,
                 float tol, const std::string& what) {
  std::vector<float> src(size_t(sc.w) * sc.h * 4);
  sc.frame(f, src);
  float worst = 0, worstRgb = 0;
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      const float* s = &src[(size_t(y) * sc.w + x) * 4];
      const float* o = r.px(x, y);
      const float m = expect(s, x, y);
      worst = std::max(worst, std::fabs(o[3] - m));
      for (int k = 0; k < 3; ++k) worstRgb = std::max(worstRgb, std::fabs(o[k] - overlay(s[k], o[3], k)));
    }
  CHECK(r.status == kOfxStatOK, what + ": render status " + std::to_string(r.status));
  CHECK(worst <= tol, what + ": matte differs from the expected by " + std::to_string(worst));
  CHECK(worstRgb <= 2e-3f, what + ": image output is not the 30% magenta overlay (error " + std::to_string(worstRgb) + ")");
  return worst;
}

// ---------------------------------------------------------------- the test context

struct Ctx {
  Options opt;
  std::unique_ptr<Host> host;
  bool dev = false;     // the bundle has the Prototype page (LTXAG_DEV_BUILD)
  bool mac = false;
  std::string binary;   // the .ofx inside the bundle

  double jobTimeout() const { return opt.jobTimeout > 0 ? opt.jobTimeout : opt.tier == "full" ? 3600 : 180; }

  // A new node on a scene, the way Flame creates one (defaults from the user config).
  std::unique_ptr<Instance> node(const Scene& sc) {
    auto in = host->createInstance(sc.w, sc.h, sc.n, sc.source());
    if (dev) {
      in->setString("pythonPath", opt.jobPython);
      in->setBool("useDummy", opt.tier == "fast");
      in->setBool("enforceGate", true);
      in->setDouble("simSeconds", 2.0);
    }
    return in;
  }

  // Licence accepted (as the user would, through the param) and the runtime/models ready.
  void readyConfig(Instance* in = nullptr) {
    auto c = readConfig();
    c["models_ready"] = "systest";
    c["ltx_python"] = opt.ltxPython;
    if (!opt.modelDir.empty()) c["model_dir"] = opt.modelDir;
    if (!opt.fp16.empty()) c["transformer_fp16"] = opt.fp16;
    writeConfig(c);
    if (in && !in->getInt("acceptLicence")) in->setBool("acceptLicence", true);
  }
};

// ---------------------------------------------------------------- fast tier

const std::vector<std::string> kExpectedParams = {
    "inputColour", "colourSpace", "process", "status", "showMarker", "precision", "unloadAfterRun", "headroomGB",
    "saveAsDefault", "viewLicence", "acceptLicence", "tokenCaption", "hfToken", "download", "modelDir",
    "existingModels", "setupStatus", "debugLogging", "collectLogs", "chunkSize", "mergeMode", "roiMargin",
    "roiMaskOutside", "roiLimit", "roiInfo", "cacheDir", "cacheInfo", "purgeCache", "purgeAll", "frameHold", "simSeconds",
    "simulate", "simNeedGB", "pythonPath", "enforceGate", "autoRefresh", "useDummy", "initialized", "nodeId",
    "refreshTick", "roiState", "version"};

void testDescribe(Ctx& c) {
  OfxPlugin* p = c.host->plugin();
  REQUIRE(p, "plugin table");
  CHECK(std::strcmp(p->pluginIdentifier, "com.janklier.ltx.alphagen") == 0, "plugin id");
  CHECK(std::strcmp(p->pluginApi, kOfxImageEffectPluginApi) == 0, "plugin api");
  note("plugin " + std::string(p->pluginIdentifier) + " v" + std::to_string(p->pluginVersionMajor) + "." +
       std::to_string(p->pluginVersionMinor));
  Effect& d = c.host->descriptor();
  CHECK(d.props.getStr(kOfxImageEffectPropSupportedContexts, 0) == kOfxImageEffectContextFilter,
        "Filter is the only context (decision 63)");
  CHECK(d.props.getStr(kOfxImageEffectPropSupportedContexts, 1).empty(), "no second context");
  CHECK(d.clips.count(kOfxImageEffectSimpleSourceClipName) && d.clips.count(kOfxImageEffectOutputClipName),
        "Source and Output clips");
  CHECK(d.clips.size() == 2, "exactly two clips (no ROI input, decision 63)");
  for (const auto& n : kExpectedParams) CHECK(d.params.byName.count(n), "param " + n + " defined");
  // Release builds hide the Prototype params but still define them (saved setups load).
  for (const char* n : {"simSeconds", "simulate", "simNeedGB", "pythonPath", "enforceGate", "autoRefresh", "useDummy"}) {
    if (!d.params.byName.count(n)) continue;
    const int secret = d.params.byName[n]->props.getInt(kOfxParamPropSecret);
    CHECK(secret == (c.dev ? 0 : 1), std::string(n) + (c.dev ? " visible in a dev build" : " hidden in a release build"));
  }
  // Input Colour (docs/colour-auto.md): Auto / Bypass, default Bypass (old setups), a clip
  // preferences slave; the old dropdown hidden but defined; OpenFX 1.5 OCIO colour management.
  if (d.params.byName.count("inputColour") && d.params.byName.count("colourSpace")) {
    const PropSet& ic = d.params.byName["inputColour"]->props;
    CHECK(ic.getStr(kOfxParamPropChoiceOption, 0) == "Auto" && ic.getStr(kOfxParamPropChoiceOption, 1) == "Bypass" &&
              ic.getStr(kOfxParamPropChoiceOption, 2).empty(),
          "Input Colour options Auto, Bypass");
    CHECK(ic.getInt(kOfxParamPropDefault) == 1, "Input Colour default Bypass (setups saved before it)");
    CHECK(d.params.byName["colourSpace"]->props.getInt(kOfxParamPropSecret) == 1, "old Input Colour hidden");
    bool slave = false;
    for (int i = 0; i < 64; ++i) slave = slave || d.props.getStr(kOfxImageEffectPropClipPreferencesSlaveParam, i) == "inputColour";
    CHECK(slave, "Input Colour is a clip preferences slave param");
  }
  CHECK(d.props.getStr("OfxImageEffectPropColourManagementStyle") == "OfxImageEffectColourManagementOCIO",
        "OCIO colour management declared");
  if (d.params.byName.count("version")) {
    const std::string v = d.params.byName["version"]->props.getStr(kOfxParamPropDefault, 0);
    CHECK(contains(v, std::to_string(p->pluginVersionMajor) + "." + std::to_string(p->pluginVersionMinor) + ".") &&
              contains(v, "("),
          "Version label shows version and git: " + v);
    note("version label: " + v);
  }
  note(std::string(c.dev ? "dev" : "release") + " build, " + std::to_string(d.params.list.size()) + " params");
}

void testCreate(Ctx& c) {
  writeConfig({});  // a user who has not set anything up yet
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  CHECK(in->getInt("initialized") == 1, "new node marked initialized");
  const std::string id = in->getString("nodeId");
  CHECK(id.size() == 8 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "nodeId is 8 hex: " + id);
  CHECK(in->echoes > 0, "the host echoed the plugin's setValues");
  CHECK(in->maxEchoDepth <= 2, "echoes did not nest (plugin ignores plugin edits): depth " +
                                   std::to_string(in->maxEchoDepth));
  CHECK(in->getString("setupStatus") == "Licence: NO | Models: NO",
        "setup status on a fresh account: " + in->getString("setupStatus"));
  CHECK(contains(in->getString("cacheDir"), home()), "cache folder defaults to the user's home");
  CHECK(in->getString("cacheInfo") == "0.0 GB used", "cache info: " + in->getString("cacheInfo"));
  // Input Colour: a new node is Auto and asks for ACEScg on the Source; Bypass asks for nothing.
  const char* kPref = "OfxImageClipPropPreferredColourspaces_Source";
  CHECK(in->getInt("inputColour") == 0, "new node: Input Colour Auto");
  CHECK(in->clipPreferences().getStr(kPref) == "ACEScg", "Auto requests ACEScg for the Source");
  in->setInt("inputColour", 1);
  PropSet bypass = in->clipPreferences();
  CHECK(!bypass.props.count(kPref), "Bypass requests nothing");
  CHECK(bypass.getStr("OfxImageClipPropComponents_Output") == kOfxImageComponentAlpha,
        "clip preferences still ask for an Alpha output");
  note("createInstance: " + std::to_string(in->echoes) + " echoed setValues, depth " + std::to_string(in->maxEchoDepth));
}

// Setups saved with the old Input Colour dropdown (colourSpace, index stored): Bypass stays
// Bypass, a colour space moves to Auto once (the old param is reset).
void testColourMigration(Ctx& c) {
  writeConfig({});
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  for (int legacy : {0, 4}) {
    c.host->savedValues = {{"initialized", 1}, {"colourSpace", legacy}};
    auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
    c.host->savedValues.clear();
    const int want = legacy == 0 ? 1 : 0;
    CHECK(in->getInt("inputColour") == want, "old colourSpace " + std::to_string(legacy) + " -> Input Colour " +
                                                 std::to_string(in->getInt("inputColour")));
    CHECK(in->getInt("colourSpace") == 0, "old param reset after the move");
  }
  // A setup saved with the new param keeps it.
  c.host->savedValues = {{"initialized", 1}, {"inputColour", 1}};
  auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  c.host->savedValues.clear();
  CHECK(in->getInt("inputColour") == 1, "saved Bypass stays Bypass");
}

void testGate(Ctx& c) {
  writeConfig({});
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  {
    auto cfg = readConfig();
    cfg["models_ready"] = "systest";
    cfg["ltx_python"] = c.opt.ltxPython;
    writeConfig(cfg);
  }
  const Rendered r = in->render(0);
  CHECK(contains(in->persistent, "Setup required"), "footer says setup required: " + in->persistent);
  CHECK(r.status == kOfxStatOK, "render ok");
  bool grey = true;  // decision 59: the image output shows the marker too, grey on black
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      const float* p = r.px(x, y);
      if (p[0] != p[3] || p[1] != p[3] || p[2] != p[3]) grey = false;
    }
  CHECK(grey, "SETUP REQUIRED: image output = the marker (decision 59)");
  checkMarker(r, "SETUP REQUIRED");
  CHECK(!in->enabled("process"), "Process disabled while the gate is closed");
  const size_t m = in->messages.size();
  in->press("process");
  CHECK(in->messages.size() == m + 1 && contains(in->messages.back().text, "Complete the Licence"),
        "Process refused with a message");
  in->setBool("acceptLicence", true);
  const auto cfg = readConfig();
  CHECK(cfg.count("licence_accepted") && cfg.count("licence_hash"), "acceptance written to the user config");
  CHECK(in->getString("setupStatus") == "Licence: OK | Models: OK", "setup status: " + in->getString("setupStatus"));
  CHECK(in->enabled("process"), "Process enabled once the gate opens");
  in->render(0);
  CHECK(contains(in->persistent, "Not processed"), "after setup: NOT PROCESSED, footer: " + in->persistent);
}

void testNotProcessed(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  const Rendered r = in->render(3);
  CHECK(contains(in->persistent, "Not processed"), "footer: " + in->persistent);
  CHECK(r.status == kOfxStatOK, "never processed: render ok");
  const AlphaStats a = alphaStats(r);
  CHECK(a.min == 1.0f && a.max == 1.0f, "never processed: white matte (decision 71)");
  std::vector<float> src(size_t(sc.w) * sc.h * 4);
  sc.frame(3, src);
  float worst = 0;
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x)
      for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(r.px(x, y)[k] - src[(size_t(y) * sc.w + x) * 4 + k]));
  CHECK(worst == 0.0f, "image output is the plain source without a matte (decision 58), error " + std::to_string(worst));
}

void testProcess(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 50, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  in->setInt("chunkSize", 3);  // Quarter (33, overlap 12): 50 frames -> 2 chunks
  bool sawProcessing = false;
  Job j = runJob(*in, c.jobTimeout(), [&](Job&) {
    if (!sawProcessing) {
      in->render(10);  // Flame renders right after the button press (decision 49)
      sawProcessing = contains(in->persistent, "Processing: this frame is not ready");
    }
  });
  REQUIRE(j.state == "done", "job state " + j.state + " " + j.message);
  CHECK(sawProcessing, "PROCESSING shown while the job ran");
  CHECK(jsonField(j.json, "frames_done") == "50", "frames_done " + jsonField(j.json, "frames_done"));
  CHECK(jsonField(j.json, "chunks") == "2", "two chunks: " + jsonField(j.json, "chunks"));
  const std::string prog = fakeProgress();
  CHECK(contains(prog, "Chunk 2/2"), "progress window showed chunk 2/2");
  CHECK(contains(in->getString("status"), "Done: 50 frames"), "status param: " + in->getString("status"));
  CHECK(in->getInt("refreshTick") > 0, "refresh tick advanced from the watcher thread");
  float worst = 0;
  for (int f = 0; f < sc.n; ++f) {
    const Rendered r = in->render(f);
    CHECK(in->persistent.empty(), "frame " + std::to_string(f) + " footer: " + in->persistent);
    worst = std::max(worst, checkMatte(r, sc, f, [](const float* s, int, int) { return luma(s); }, 4e-3f,
                                       "frame " + std::to_string(f)));
  }
  note("50 frames, 2 chunks, " + std::to_string(int(j.seconds)) + " s, worst matte error " + std::to_string(worst));
  checkClean(*in);
  CHECK(contains(in->getString("cacheInfo"), "GB used"), "cache info updated");

  // Flow Warp merge (decision 62): identical chunk mattes -> a plain blend, same result.
  in->setInt("mergeMode", 1);
  j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "flow merge job " + j.state + " " + j.message);
  for (int f : {20, 25, 30})
    checkMatte(in->render(f), sc, f, [](const float* s, int, int) { return luma(s); }, 4e-3f,
               "flow merge frame " + std::to_string(f));

  // Input Colour is part of the cache key: the mode, and with Auto the colourspace the host
  // delivered (this host has no colour management: no tag).
  in->setInt("inputColour", 1);
  checkFailed(in->render(5), "Bypass on a node processed with Auto");
  CHECK(contains(in->persistent, "Matte missing"), "another Input Colour misses the cache: " + in->persistent);
  in->setInt("inputColour", 0);
  in->render(5);
  CHECK(in->persistent.empty(), "back to Auto: cache hit again");
  PropSet& srcClip = in->effect().clips[kOfxImageEffectSimpleSourceClipName]->props;
  srcClip.setStr("OfxImageClipPropColourspace", "ACEScg");
  checkFailed(in->render(5), "Auto with another delivered colourspace");
  CHECK(contains(in->persistent, "Matte missing"), "another delivered colourspace misses the cache");
  srcClip.props.erase("OfxImageClipPropColourspace");
  in->render(5);
  CHECK(in->persistent.empty(), "delivered colourspace back: cache hit again");

  // Frame Hold (decision 74) is part of the key (Off keeps the old one); 2x halves a chunk's
  // source frames: Quarter 33 -> 17 frames, overlap 6 -> 4 chunks for 50 frames.
  in->setInt("frameHold", 2);  // Off, Auto, 2x, 4x, 8x
  checkFailed(in->render(5), "2x frame hold on a processed node");
  CHECK(contains(in->persistent, "Matte missing"), "2x frame hold misses the cache: " + in->persistent);
  j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "2x frame hold job " + j.state + " " + j.message);
  CHECK(jsonField(j.json, "chunks") == "4", "2x frame hold: four chunks: " + jsonField(j.json, "chunks"));
  for (int f : {0, 17, 49})
    checkMatte(in->render(f), sc, f, [](const float* s, int, int) { return luma(s); }, 4e-3f,
               "2x frame hold frame " + std::to_string(f));
  in->setInt("frameHold", 0);
  in->render(5);
  CHECK(in->persistent.empty(), "frame hold Off: the earlier mattes hit again");

  // Purge Cache (this node), then Purge All.
  // Purge All (or a swept or missing cache): the node still remembers it was processed.
  in->press("purgeAll");
  CHECK(in->getString("cacheInfo") == "0.0 GB used", "purge all: " + in->getString("cacheInfo"));
  checkFailed(in->render(5), "processed node, cache gone");
  CHECK(contains(in->persistent, "Matte missing"), "footer: " + in->persistent);
  in->setBool("showMarker", false);
  checkFailed(in->render(5), "Show Marker off does not hide MATTE MISSING");
  in->setBool("showMarker", true);
  // Purge Cache (this node) resets it to never processed: pass-through.
  in->press("purgeCache");
  CHECK(contains(in->getString("status"), "Cache purged"), "purge status: " + in->getString("status"));
  const Rendered r = in->render(5);
  CHECK(r.status == kOfxStatOK && alphaStats(r).min == 1.0f, "purged node passes through with a white matte");
  CHECK(contains(in->persistent, "Not processed"), "purged frame not processed: " + in->persistent);
}

// Decision 75: Auto Frame Hold measures the motion while staging and holds only the chunks that
// need it. A clip whose second half jumps 60 px per frame gets 1x and 4x chunks; a slow clip gets
// the Off plan.
void testAutoHold(Ctx& c) {
  c.readyConfig();
  const std::string logPath = home() + "/.cache/ltx-alphagen/logs/ltx-alphagen.log";
  auto planLine = [&]() {
    const std::string log = readFile(logPath);
    const size_t pos = log.rfind("auto hold: motion px/frame");
    return pos == std::string::npos ? std::string() : log.substr(pos, log.find('\n', pos) - pos);
  };
  Scene fast{320, 240, 50, AlphaMode::Opaque};
  fast.jump = 60.0;
  auto in = c.node(fast);
  c.readyConfig(in.get());
  in->setInt("chunkSize", 3);  // Quarter: 33 model frames per chunk
  in->setInt("frameHold", 1);  // Auto
  Job j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "auto hold job " + j.state + " " + j.message);
  std::string line = planLine();
  note(line);
  CHECK(contains(line, "@1x") && contains(line, "@4x"), "fast second half: 1x and 4x chunks: " + line);
  CHECK(std::atoi(jsonField(j.json, "chunks").c_str()) > 2, "more chunks than Off: " + jsonField(j.json, "chunks"));
  CHECK(contains(fakeProgress(), "4x hold"), "progress window names the hold");
  for (int f : {0, 20, 30, 49})
    checkMatte(in->render(f), fast, f, [](const float* s, int, int) { return luma(s); }, 4e-3f,
               "auto hold frame " + std::to_string(f));
  checkClean(*in);

  Scene slow{320, 240, 50, AlphaMode::Opaque};
  auto in2 = c.node(slow);
  in2->setInt("chunkSize", 3);
  in2->setInt("frameHold", 1);
  j = runJob(*in2, c.jobTimeout());
  REQUIRE(j.state == "done", "auto hold slow job " + j.state + " " + j.message);
  line = planLine();
  note(line);
  CHECK(jsonField(j.json, "chunks") == "2" && !contains(line, "@2x") && !contains(line, "@4x"),
        "slow clip: the Off plan (2 chunks at 1x): " + line);
  checkClean(*in2);
}

void testRoi(Ctx& c) {
  c.readyConfig();
  Scene sc{1024, 576, 17, AlphaMode::Roi, 0.45};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  in->render(0);
  CHECK(contains(in->persistent, "Not processed"), "alpha matte before Process: " + in->persistent);
  Job j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "ROI job " + j.state + " " + j.message);
  const std::string info = in->getString("roiInfo");
  int bx = 0, by = 0, bw = 0, bh = 0, act = 0, mask = 0;
  std::sscanf(in->getString("roiState").c_str(), "%d,%d,%d,%d,%d,%d", &act, &bx, &by, &bw, &bh, &mask);
  CHECK(act == 1 && bw > 0 && bh > 0, "roiState records an active box: " + in->getString("roiState"));
  CHECK(info == "Crop " + std::to_string(bw) + "x" + std::to_string(bh), "ROI info: " + info);
  CHECK(bw % 32 == 0 && bh % 32 == 0, "crop is a multiple of 32");
  CHECK(bw >= std::min(512, sc.w) && bh >= std::min(512, sc.h), "crop at least 512 per side (or the image)");
  CHECK(bx >= 0 && by >= 0 && bx + bw <= sc.w && by + bh <= sc.h, "crop inside the image");
  CHECK(bw < sc.w, "the crop is smaller than the frame (the test clip leaves room)");
  note("crop x " + std::to_string(bx) + " y " + std::to_string(by) + " " + std::to_string(bw) + "x" + std::to_string(bh));
  auto inBox = [&](int x, int y) { return x >= bx && x < bx + bw && y >= by && y < by + bh; };
  for (int f : {0, 8, 16})
    checkMatte(in->render(f), sc, f, [&](const float* s, int x, int y) { return inBox(x, y) ? luma(s) : 0.0f; },
               4e-3f, "ROI frame " + std::to_string(f));
  // Multiply Result by ROI: at render time, no reprocessing.
  in->setBool("roiLimit", true);
  checkMatte(in->render(8), sc, 8,
             [&](const float* s, int x, int y) { return inBox(x, y) ? luma(s) * std::min(1.0f, s[3]) : 0.0f; }, 4e-3f,
             "Multiply Result by ROI");
  in->setBool("roiLimit", false);
  // Black Out Outside ROI changes the matte: the old result no longer counts.
  in->setBool("roiMaskOutside", true);
  checkFailed(in->render(8), "changed ROI settings");
  CHECK(contains(in->persistent, "Matte missing"), "changed ROI settings need Process: " + in->persistent);
  j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "black-out job " + j.state);
  checkMatte(in->render(8), sc, 8,
             [&](const float* s, int x, int y) {
               if (!inBox(x, y)) return 0.0f;
               const float a = std::min(1.0f, std::max(0.0f, s[3]));
               const float b[3] = {s[0] * a, s[1] * a, s[2] * a};
               return luma(b);
             },
             4e-3f, "Black Out Outside ROI");
  checkClean(*in);
  // An empty alpha is an error, not a crop of nothing.
  Scene empty{320, 240, 9, AlphaMode::Empty};
  auto e = c.node(empty);
  j = runJob(*e, c.jobTimeout());
  CHECK(j.state == "none" && contains(j.message, "source alpha is empty"), "empty alpha refused: " + j.message);
}

void testCancel(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 33, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  in->setDouble("simSeconds", 120.0);
  bool asked = false;
  double tCancel = 0;
  Job j = runJob(*in, c.jobTimeout(), [&](Job&) {
    if (!asked && contains(fakeProgress(), "#")) {  // the window shows progress: the job runs
      requestCancel();
      asked = true;
      tCancel = now();
    }
  });
  CHECK(asked, "the progress window appeared");
  CHECK(j.state == "cancelled", "job state " + j.state + " " + j.message);
  if (asked) {
    note("cancelled " + std::to_string(now() - tCancel).substr(0, 4) + " s after the Cancel press");
    CHECK(now() - tCancel < 30, "cancel took under 30 s");
  }
  in->render(3);
  CHECK(contains(in->persistent, "cancelled"), "footer after cancel: " + in->persistent);
  checkFailed(in->render(3), "CANCELLED");
  checkClean(*in);
  clearFakeUi();
}

void testFailures(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  struct Case {
    int sim;
    const char* expect;
  };
  for (const Case& k : {Case{1, "out of memory"}, Case{2, "ended"}}) {
    in->setInt("simulate", k.sim);
    Job j = runJob(*in, c.jobTimeout());
    CHECK(j.state == "failed", std::string("simulate ") + std::to_string(k.sim) + ": state " + j.state);
    CHECK(contains(j.message, k.expect) || contains(j.message, "signal") || contains(j.message, "exit"),
          "failure message: " + j.message);
    in->render(2);
    CHECK(contains(in->persistent, "Processing failed"), "footer: " + in->persistent);
    checkFailed(in->render(2), "FAILED");
    checkClean(*in);
    note("simulate " + std::to_string(k.sim) + ": " + j.message);
    closeMacDialogs();
  }
#ifndef __APPLE__
  CHECK(contains(readFile(fakeUiDir() + "/messages.log"), "Processing failed"), "job.py showed an error window");
#endif
  in->setInt("simulate", 0);
}

// Decision 72: a 6K crop is refused at Process with advice; an alpha ROI that crops it to a
// smaller box processes normally.
void testTooLarge(Ctx& c) {
  c.readyConfig();
  Scene full{6144, 3264, 1, AlphaMode::Opaque};
  auto in = c.node(full);
  c.readyConfig(in.get());
  Job j = runJob(*in, c.jobTimeout());
  CHECK(j.state == "none", "6K full frame: no job launched (" + j.state + ")");
  CHECK(contains(j.message, "6144x3264") && contains(j.message, "6K") && contains(j.message, "region of interest"),
        "6K refusal names the size and the way out: " + j.message);
  Scene roi{6144, 3264, 1, AlphaMode::Roi, 0.3};
  auto in2 = c.node(roi);
  c.readyConfig(in2.get());
  j = runJob(*in2, c.jobTimeout());
  CHECK(j.state == "done", "6K frame with a small alpha ROI processes: " + j.state + " " + j.message);
  note("ROI crop " + in2->getString("roiInfo"));
}

void testMemoryWarning(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  in->setDouble("simNeedGB", 100000.0);
  in->replies = {Reply::No};
  const size_t m = in->messages.size();
  Job j = runJob(*in, c.jobTimeout());
  bool asked = false;
  for (size_t i = m; i < in->messages.size(); ++i)
    if (in->messages[i].type == kOfxMessageQuestion && contains(in->messages[i].text, "may not fit")) asked = true;
  if (!asked) {
    CHECK(j.state == "done", "without GPU memory info the job just runs: " + j.state);
    throw Skip("no GPU memory information on this machine: the warning cannot appear");
  }
  CHECK(j.state == "none", "No: no job launched (" + j.state + ")");
  in->replies = {Reply::Yes};
  j = runJob(*in, c.jobTimeout());
  CHECK(j.state == "done", "Yes: the job runs: " + j.state);
  in->replies.clear();
  in->defaultReply = Reply::None;  // a host that doesn't ask: run, with a footer warning
  j = runJob(*in, c.jobTimeout());
  CHECK(j.state == "done", "no answer: the job runs: " + j.state);
  CHECK(contains(in->persistent, "may not fit"), "no answer: footer warning (decision 56): " + in->persistent);
  in->setDouble("simNeedGB", 0.0);
}

void testDefaults(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto a = c.node(sc);
  c.readyConfig(a.get());
  a->setInt("precision", c.mac ? 0 : 1);
  a->setDouble("headroomGB", 3.5);
  a->setBool("unloadAfterRun", false);
  a->press("saveAsDefault");
  CHECK(!a->messages.empty() && contains(a->messages.back().text, "Saved as your default"), "save confirmation");
  const auto cfg = readConfig();
  CHECK(cfg.count("headroom_gb") && cfg.at("headroom_gb") == "3.5", "config headroom_gb");
  CHECK(cfg.count("unload") && cfg.at("unload") == "0", "config unload");
  auto b = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  CHECK(std::fabs(b->getDouble("headroomGB") - 3.5) < 1e-9, "a new node takes the saved reserve");
  CHECK(b->getInt("unloadAfterRun") == 0, "a new node takes the saved Unload GPU");
  CHECK(b->getInt("precision") == (c.mac ? 0 : 1), "a new node takes the saved precision");
  auto cfg2 = readConfig();
  cfg2.erase("headroom_gb");
  cfg2.erase("unload");
  cfg2.erase("precision");
  writeConfig(cfg2);
}

void testMacFp8(Ctx& c) {
  if (!c.mac) throw Skip("fp8 is refused on Apple Silicon only");
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  c.readyConfig(in.get());
  in->setInt("precision", 1);
  Job j = runJob(*in, c.jobTimeout());
  CHECK(j.state == "none" && contains(j.message, "fp8 is not available on Apple Silicon"), "fp8 refused: " + j.message);
}

void testCollectLogs(Ctx& c) {
  c.readyConfig();
  Scene sc{320, 240, 9, AlphaMode::Opaque};
  auto in = c.node(sc);
  in->press("collectLogs");
  REQUIRE(!in->messages.empty(), "a message after Collect Logs");
  const std::string t = in->messages.back().text;
  CHECK(contains(t, "Support bundle written to "), "message: " + t);
  const auto a = t.find(home() + "/ltx-alphagen-support-");
  const auto b = t.find(".tar.gz", a);
  CHECK(a != std::string::npos && b != std::string::npos && exists(t.substr(a, b + 7 - a)), "bundle file exists");
  const std::string log = readFile(home() + "/.cache/ltx-alphagen/logs/ltx-alphagen.log");
  CHECK(contains(log, "==== plugin load (describe) ===="), "plugin log written in the isolated HOME");
  CHECK(contains(log, "job["), "job.py logged into the same file");
}

// ---------------------------------------------------------------- full tier

void testFullGate(Ctx& c) {
  writeConfig({});
  c.readyConfig();  // runtime and models ready, licence not yet accepted
  Scene sc{512, 512, 33, AlphaMode::Opaque};
  auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  if (c.dev) in->setBool("enforceGate", true);
  in->render(0);
  CHECK(contains(in->persistent, "Setup required"), "release gate closed without the licence: " + in->persistent);
  in->setBool("acceptLicence", true);
  CHECK(in->getString("setupStatus") == "Licence: OK | Models: OK", "setup status: " + in->getString("setupStatus"));
  in->render(0);
  CHECK(contains(in->persistent, "Not processed"), "gate open: " + in->persistent);
}

bool workerAlive() {
  const std::string info = readFile(home() + "/.cache/ltx-alphagen/run/worker.sock.json");
  const int pid = std::atoi(jsonField(info, "pid").c_str());
  return pid > 0 && kill(pid, 0) == 0;
}

void testRealMatte(Ctx& c) {
  c.readyConfig();
  Scene sc{512, 512, 33, AlphaMode::Opaque};
  auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  c.readyConfig(in.get());
  in->setInt("precision", 0);  // bf16 (the only one on a Mac)
  in->setBool("unloadAfterRun", true);
  in->setInt("chunkSize", 3);  // Quarter: one chunk of 33
  in->defaultReply = Reply::Yes;  // the memory warning, if this GPU looks too small
  const auto csv = home() + "/.cache/ltx-alphagen/logs/vram_stats.csv";
  Job j = runJob(*in, c.jobTimeout());
  REQUIRE(j.state == "done", "real job " + j.state + " " + j.message);
  // Decision 68: on a Mac whose Setup made the fp16 copy, the dropdown's bf16 runs as fp16.
  const std::string log = readFile(home() + "/.cache/ltx-alphagen/logs/ltx-alphagen.log");
  const std::string ran = contains(log, "prec=fp16|") ? "fp16" : "bf16";
  CHECK(ran == (c.opt.fp16 == "1" ? "fp16" : "bf16"),
        "precision that ran (" + ran + ") matches transformer_fp16=" + c.opt.fp16);
  note("real model: 33 frames 512x512 " + ran + " in " + std::to_string(int(j.seconds)) + " s");
  double inside = 0, outside = 0, err = 0;
  long nin = 0, nout = 0, nall = 0;
  for (int f = 0; f < sc.n; ++f) {
    const Rendered r = in->render(f);
    CHECK(in->persistent.empty(), "frame " + std::to_string(f) + " footer: " + in->persistent);
    for (int y = 0; y < r.h; ++y)
      for (int x = 0; x < r.w; ++x) {
        const float t = sc.truth(f, x, y), a = r.px(x, y)[3];
        if (t > 0.99f) inside += a, ++nin;
        if (t < 0.01f) outside += a, ++nout;
        err += std::fabs(a - t), ++nall;
      }
  }
  inside /= std::max(1L, nin);
  outside /= std::max(1L, nout);
  err /= std::max(1L, nall);
  note("matte: inside mean " + std::to_string(inside) + ", outside mean " + std::to_string(outside) +
       ", mean abs error vs truth " + std::to_string(err));
  CHECK(inside > 0.8 && outside < 0.1, "the matte separates the subject");
  CHECK(err < 0.05, "mean abs error vs truth below 0.05");
  CHECK(contains(readFile(csv), "\n"), "a VRAM record in logs/vram_stats.csv (decision 56)");
  double t0 = now();
  while (workerAlive() && now() - t0 < 60) sleepMs(500);
  CHECK(!workerAlive(), "Unload GPU on: no worker left after the job");
  checkClean(*in);
}

void testRealCancel(Ctx& c) {
  c.readyConfig();
  Scene sc{512, 512, 33, AlphaMode::Opaque};
  auto in = c.host->createInstance(sc.w, sc.h, sc.n, sc.source());
  c.readyConfig(in.get());
  in->setInt("precision", 0);
  in->setBool("unloadAfterRun", true);
  in->setInt("chunkSize", 3);
  in->defaultReply = Reply::Yes;
  in->press("purgeCache");  // else the earlier test's mattes would hide the CANCELLED state
  bool asked = false;
  std::string phase;
  double tCancel = 0;
  Job j = runJob(*in, c.jobTimeout(), [&](Job&) {
    // Once the model is loaded and the chunk's GPU work has begun (reference encode, then the
    // steps; a fast GPU does a 512x512 step in ~0.3 s, faster than the window updates).
    const std::string prog = fakeProgress();
    if (!asked && contains(prog, "Chunk 1/")) {
      requestCancel();
      asked = true;
      tCancel = now();
      phase = prog.substr(prog.rfind("Chunk 1/"));
      phase = phase.substr(0, phase.find('\n'));
    }
  });
  REQUIRE(asked, "the chunk never started (state " + j.state + " " + j.message + ")");
  CHECK(j.state == "cancelled", "state " + j.state + " " + j.message);
  note("cancelled " + std::to_string(now() - tCancel).substr(0, 4) + " s after the press, window showed: " + phase);
  double t0 = now();
  while (workerAlive() && now() - t0 < 30) sleepMs(250);
  CHECK(!workerAlive(), "the worker is gone within 30 s of Cancel");
  in->render(3);
  CHECK(contains(in->persistent, "cancelled"), "footer: " + in->persistent);
  checkClean(*in);
  clearFakeUi();
}

// ---------------------------------------------------------------- driver

struct Test {
  const char* name;
  void (*fn)(Ctx&);
};

std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '<') o += "&lt;";
    else if (ch == '>') o += "&gt;";
    else if (ch == '&') o += "&amp;";
    else if (ch == '"') o += "&quot;";
    else o += ch;
  }
  return o;
}

std::string bundleBinary(const std::string& bundle) {
  for (const char* arch : {"MacOS", "Linux-x86-64"}) {
    const std::string p = bundle + "/Contents/" + arch + "/LTXAlphaGen.ofx";
    if (exists(p)) return p;
  }
  return "";
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--bundle") o.bundle = v;
    else if (k == "--tier") o.tier = v;
    else if (k == "--out") o.out = v;
    else if (k == "--ltx-python") o.ltxPython = v;
    else if (k == "--model-dir") o.modelDir = v;
    else if (k == "--job-python") o.jobPython = v;
    else if (k == "--fp16") o.fp16 = v;
    else if (k == "--job-timeout") o.jobTimeout = std::atof(v.c_str());
    else if (k == "--only") {
      std::stringstream ss(v);
      std::string t;
      while (std::getline(ss, t, ',')) o.only.insert(t);
    } else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (o.bundle.empty() || o.ltxPython.empty() || (o.tier != "fast" && o.tier != "full") || fakeUiDir().empty()) {
    std::fprintf(stderr, "usage: ltxag_systest --bundle B --tier fast|full --out DIR --ltx-python P "
                         "[--model-dir D] [--job-python P] [--job-timeout S] [--only a,b]\n"
                         "(LTXAG_FAKE_UI_DIR must be set: run it through systest/run_systest.sh)\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  Ctx c;
  c.opt = o;
#ifdef __APPLE__
  c.mac = true;
#endif
  c.binary = bundleBinary(o.bundle);
  const std::vector<Test> fast = {
      {"describe", testDescribe},     {"create_instance", testCreate},   {"colour_migration", testColourMigration},   {"gate", testGate},
      {"not_processed", testNotProcessed}, {"process_chunks_merge_purge", testProcess},
      {"auto_hold", testAutoHold},
      {"roi", testRoi},               {"cancel", testCancel},            {"failures", testFailures},
      {"too_large", testTooLarge},    {"memory_warning", testMemoryWarning}, {"save_default", testDefaults}, {"mac_fp8_refused", testMacFp8},
      {"collect_logs", testCollectLogs}};
  const std::vector<Test> full = {
      {"describe", testDescribe}, {"release_gate", testFullGate}, {"real_matte", testRealMatte},
      {"real_cancel", testRealCancel}, {"collect_logs", testCollectLogs}};
  const auto& tests = o.tier == "fast" ? fast : full;

  std::vector<Result> results;
  const double t0 = now();
  std::printf("ltxag_systest: tier %s, bundle %s, HOME %s\n", o.tier.c_str(), o.bundle.c_str(), home().c_str());
  {
    Result load{"load"};
    gCur = &load;
    const double ts = now();
    if (c.binary.empty()) {
      fail("no LTXAlphaGen.ofx in " + o.bundle);
    } else {
      c.host.reset(new Host(c.binary));
      if (!c.host->loaded()) fail("load: " + c.host->loadError());
      else if (OfxStatus s = c.host->describe()) {
        std::string props;
        for (const auto& n : c.host->unknownProps) props += " " + n;
        fail("load/describe returned " + std::to_string(s) + "; unknown properties asked for:" + props);
      }
      else c.dev = c.host->descriptor().params.byName.count("simSeconds") &&
                   !c.host->descriptor().params.byName["simSeconds"]->props.getInt(kOfxParamPropSecret);
    }
    load.seconds = now() - ts;
    results.push_back(load);
    if (!load.failures.empty()) std::printf("  load FAILED: %s\n", load.failures[0].c_str());
  }
  if (results[0].failures.empty()) {
    if (o.tier == "fast" && !c.dev) {
      std::fprintf(stderr, "the fast tier needs a dev build (LTXAG_DEV_BUILD=ON) for the dummy worker\n");
      return 2;
    }
    for (const auto& t : tests) {
      if (!o.only.empty() && !o.only.count(t.name)) continue;
      Result r{t.name};
      gCur = &r;
      std::printf("  %s\n", t.name);
      std::fflush(stdout);
      const size_t errs = c.host->errors.size();
      const double ts = now();
      try {
        t.fn(c);
      } catch (const Skip& e) {
        r.skipped = e.what();
      } catch (const std::exception& e) {
        r.failures.push_back(std::string("aborted: ") + e.what());
      }
      std::set<std::string> seen;  // a recursion repeats the same error many times
      for (size_t i = errs; i < c.host->errors.size(); ++i)
        if (seen.insert(c.host->errors[i]).second) r.failures.push_back("host: " + c.host->errors[i]);
      r.seconds = now() - ts;
      std::printf("  %s %s (%.1f s)%s\n", r.failures.empty() ? (r.skipped.empty() ? "PASS" : "SKIP") : "FAIL",
                  t.name, r.seconds, r.skipped.empty() ? "" : (" - " + r.skipped).c_str());
      for (const auto& f : r.failures) std::printf("      %s\n", f.c_str());
      std::fflush(stdout);
      results.push_back(r);
    }
  }

  int failed = 0, skipped = 0;
  for (const auto& r : results) failed += !r.failures.empty(), skipped += !r.skipped.empty();
  const std::string platform = c.mac ? "macos" : "linux";
  std::ofstream x(o.out + "/junit-" + o.tier + ".xml");
  x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"ltxag-systest-" << o.tier << "-" << platform
    << "\" tests=\"" << results.size() << "\" failures=\"" << failed << "\" skipped=\"" << skipped << "\" time=\""
    << now() - t0 << "\">\n";
  for (const auto& r : results) {
    x << "  <testcase classname=\"" << platform << "." << o.tier << "\" name=\"" << r.name << "\" time=\"" << r.seconds
      << "\">\n";
    for (const auto& f : r.failures) x << "    <failure message=\"" << xmlEscape(f) << "\"/>\n";
    if (!r.skipped.empty()) x << "    <skipped message=\"" << xmlEscape(r.skipped) << "\"/>\n";
    if (!r.notes.empty()) {
      x << "    <system-out>";
      for (const auto& n : r.notes) x << xmlEscape(n) << "\n";
      x << "</system-out>\n";
    }
    x << "  </testcase>\n";
  }
  x << "</testsuite>\n";
  x.close();  // _exit below skips the destructor, which left the file empty
  std::printf("ltxag_systest %s on %s: %zu tests, %d failed, %d skipped, %.0f s\n", o.tier.c_str(), platform.c_str(),
              results.size(), failed, skipped, now() - t0);
  std::fflush(stdout);
  // Instances are gone; the host stays loaded (no dlclose). _exit: the plugin's statics may
  // still be referenced by detached threads.
  c.host.reset();
  _exit(failed ? 1 : 0);
}
