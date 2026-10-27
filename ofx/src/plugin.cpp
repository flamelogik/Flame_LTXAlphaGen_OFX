// LTX Alpha Gen OFX — PROTOTYPE round 3 (CLAUDE.md decisions 35–43).
// - Render never runs the model: it serves mattes from the disk cache, else a faint
//   "no media"-style marker (Flame calls render on its UI thread — Flame test 1).
// - Process stages the clip's frames in stagingDir() and launches a detached job
//   (worker/job.py) with its own progress window and Cancel button.
// - A watcher thread follows the job and nudges Flame to re-render (decision 42, option a:
//   setting params outside an action breaks the OFX threading rules — experiment, toggleable).
// - Layout follows Flame's observed rules: one group = one column of <= 5 rows.
// - Licence + model gating (bypassable only in dev builds). Logging per decision 39.
// Not in the prototype: OCIO (colour space only feeds the cache key), real model, resident worker.

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "cache.h"
#include "chunk_plan.h"
#include "motion.h"
#include "env_info.h"
#include "half.h"
#include "log.h"
#include "marker.h"
#include "ofxColour.h"
#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"
#include "platform.h"
#include "process.h"
#include "roi.h"
#include "transport.h"
#include "user_config.h"
#include "vram_estimate.h"

#ifndef LTXAG_VERSION  // from CMakeLists.txt
#define LTXAG_VERSION "0.0.0"
#endif
#ifndef LTXAG_GIT
#define LTXAG_GIT "unknown"
#endif
#ifndef LTXAG_VERSION_MAJOR  // from CMakeLists.txt LTXAG_VERSION
#define LTXAG_VERSION_MAJOR 0
#define LTXAG_VERSION_MINOR 0
#endif
#ifndef LTXAG_DEV_BUILD
#define LTXAG_DEV_BUILD 0
#endif

namespace {

using namespace ltxag;

constexpr const char* kPluginId = "com.janklier.ltx.alphagen";
constexpr const char* kPluginLabel = "LTX Alpha Gen";
// Part of every cache key: which worker/model produced a matte (dummy and real never mix).
constexpr const char* kModelVersion = "ltx2.5-alphagen0.9-recipe1";
constexpr const char* kDummyVersion = "dummy-0.3";

// Decision 34(a): bf16 is the built-in default only if a full HD chunk fits (vram_estimate.h).
constexpr int kDefaultCheckW = 1920, kDefaultCheckH = 1080, kDefaultCheckFrames = 145;
// Decision 72: Process refuses a crop whose long side reaches this (every 6K-class format;
// 3840x2176 is measured to work, 6144x3264 fails in the LTX decoder).
constexpr int kMaxLongSide = 6000;

// Input Colour (docs/colour-auto.md): Auto asks the host (OpenFX 1.5 colour management) for the
// source in kAutoColourspace; the worker then applies the fixed ACES 2.0 SDR view, as the old
// dropdown's ACEScg entry did. Bypass: no request, no conversion. Setups store the index.
const char* const kColourModeNames[] = {"Auto", "Bypass"};
constexpr int kColourAuto = 0, kColourBypass = 1;
constexpr const char* kAutoColourspace = "ACEScg";
// Auto: the image output is display-referred (acescgToDisplay, marker.h), decision 76.
constexpr const char* kOutputDisplay = "Rec.1886 Rec.709 - Display";
// The old Input Colour dropdown (until 0.2.1): kept defined and hidden so saved setups load; a
// node saved with a colour space other than Bypass is moved to Auto once (createInstance).
const char* const kLegacyColourSpaces[] = {"Bypass", "ACEScg", "LogC4", "Log3G10", "LogC3", "BMD Film Gen5"};
const char* const kPrecisions[] = {"bf16", "fp8"};
const char* const kSimulations[] = {"none", "oom", "crash", "hang"};
// Chunk Size (decision 61): index 0 is Auto, then chunk_plan.h kSizeLen. Setups store the index.
const char* const kChunkSizeNames[] = {"Auto", "Full (145)", "Half (73)", "Quarter (33)"};
// Merge (decision 62): how overlapping chunks are joined.
const char* const kMergeNames[] = {"Smooth", "Flow Warp"};
// Frame Hold (decisions 74, 75): each frame repeated 1/2/4/8 times for fast motion; Auto (0)
// picks 1/2/4 per chunk from the measured motion. Setups store the index: 8x stays last.
const char* const kFrameHoldNames[] = {"Off", "Auto", "2x", "4x", "8x"};
const int kFrameHolds[] = {1, 0, 2, 4, 8};
constexpr int kAutoMaxHold = 4;

// Params. Column (group) in brackets; see CLAUDE.md "Revised layout".
// [Alpha Gen]
constexpr const char* kPColourMode = "inputColour";
constexpr const char* kPProcess = "process";
constexpr const char* kPStatus = "status";
constexpr const char* kPShowMarker = "showMarker";
// [Performance]
constexpr const char* kPPrecision = "precision";
constexpr const char* kPUnload = "unloadAfterRun";
constexpr const char* kPHeadroom = "headroomGB";
constexpr const char* kPSaveDefault = "saveAsDefault";
// [Licence] — per user: not saved in setups, mirrored to the user config.
constexpr const char* kPViewLicence = "viewLicence";
constexpr const char* kPAccept = "acceptLicence";
constexpr const char* kPTokenCaption = "tokenCaption";
constexpr const char* kPToken = "hfToken";
constexpr const char* kPDownload = "download";
// [Models] — per user.
constexpr const char* kPModelDir = "modelDir";
constexpr const char* kPExistingDir = "existingModels";
constexpr const char* kPSetupStatus = "setupStatus";
constexpr const char* kPDebugLog = "debugLogging";
constexpr const char* kPCollectLogs = "collectLogs";
// [Chunking]
constexpr const char* kPChunkSize = "chunkSize";
constexpr const char* kPMerge = "mergeMode";
// [Motion]
constexpr const char* kPFrameHold = "frameHold";
// [ROI]
constexpr const char* kPRoiMargin = "roiMargin";
constexpr const char* kPRoiMask = "roiMaskOutside";
constexpr const char* kPRoiLimit = "roiLimit";
constexpr const char* kPRoiInfo = "roiInfo";
// [Cache] — per user, except Purge Cache (this node).
constexpr const char* kPCacheDir = "cacheDir";
constexpr const char* kPCacheInfo = "cacheInfo";
constexpr const char* kPPurgeNode = "purgeCache";
constexpr const char* kPPurgeAll = "purgeAll";
// [Prototype] — dev builds only.
constexpr const char* kPSimSeconds = "simSeconds";
constexpr const char* kPSimulate = "simulate";
constexpr const char* kPSimNeed = "simNeedGB";
constexpr const char* kPPython = "pythonPath";
constexpr const char* kPEnforceGate = "enforceGate";
constexpr const char* kPAutoRefresh = "autoRefresh";
constexpr const char* kPUseDummy = "useDummy";
// Hidden.
constexpr const char* kPInitialized = "initialized";
constexpr const char* kPNodeId = "nodeId";
constexpr const char* kPRefreshTick = "refreshTick";
constexpr const char* kPRoiState = "roiState";
constexpr const char* kPColour = "colourSpace";  // the old Input Colour dropdown (hidden)

constexpr const char* kTokenCaptionDefault = "Hugging Face token:";

std::string resourcesDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&resourcesDir), &info) || !info.dli_fname) return "";
  std::string p = info.dli_fname;  // .../Contents/{Linux-x86-64,MacOS}/LTXAlphaGen.ofx
  p = p.substr(0, p.rfind('/'));
  p = p.substr(0, p.rfind('/'));
  return p + "/Resources";
}

std::string homeDir() {
  const char* h = std::getenv("HOME");
  return h ? h : "/tmp";
}

std::string timeStr(const char* fmt) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, fmt, &tm);
  return b;
}

// "/a/b/" -> "/a/b" (but "/" stays "/").
std::string trimSlashes(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

std::string fmtGB(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}

std::string randomId() {
  std::random_device rd;
  char b[9];
  std::snprintf(b, sizeof b, "%08x", rd());
  return b;
}

// Value of "key" in the job status JSON written by Python's json.dump.
std::string jsonField(const std::string& j, const std::string& key) {
  const std::string k = "\"" + key + "\": ";
  const auto p = j.find(k);
  if (p == std::string::npos) return "";
  const size_t b = p + k.size();
  if (b < j.size() && j[b] == '"') return j.substr(b + 1, j.find('"', b + 1) - b - 1);
  const auto e = j.find_first_of(",}", b);
  return j.substr(b, e - b);
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Licence text shown by "View Licence"; its hash is stored on acceptance (decision 38).
std::string licencePath() { return resourcesDir() + "/licence.txt"; }
std::string licenceHash() {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx",
                static_cast<unsigned long long>(fnv(readFile(licencePath()))));
  return b;
}

struct SetupState {
  bool licenceAccepted = false;
  bool modelsReady = false;
  bool ready() const { return licenceAccepted && modelsReady; }
};

SetupState setupState() {
  const UserConfig cfg = loadUserConfig();
  SetupState s;
  s.licenceAccepted = cfg.count("licence_accepted") && cfg.count("licence_hash") &&
                      cfg.at("licence_hash") == licenceHash();
  // Ready = the setup helper finished (models verified, prompt cache built) AND the runtime exists.
  struct stat st {};
  s.modelsReady = cfg.count("models_ready") > 0 && cfg.count("ltx_python") > 0 &&
                  stat(cfg.at("ltx_python").c_str(), &st) == 0;
  return s;
}

// Built-in defaults, overridden by the user config (decisions 21, 31, 32, 34).
struct Defaults {
  int precision = 0;
  double headroomGB = 0.0;
  bool unload = true;
  std::string source;
};

// Apple Silicon runs the model on PyTorch-MPS (decision 65): bf16 only (MPS has no fp8 dtype)
// and its own memory model (vram_estimate.h).
#ifdef __APPLE__
constexpr bool kMps = true;
#else
constexpr bool kMps = false;
#endif

// Decision 68: on a Mac where Setup made the fp16 copy of the transformer (because fp16 is faster
// there), the Precision choice "bf16" runs as "fp16": same matte (mean difference 0.0002), its
// own cache entries and worker. The dropdown itself stays bf16 / fp8.
std::string effectivePrecision(int prec) {
  if (kMps && prec == 0) {
    const UserConfig cfg = loadUserConfig();
    if (cfg.count("transformer_fp16") && cfg.at("transformer_fp16") == "1") return "fp16";
  }
  return kPrecisions[prec];
}

Defaults computeDefaults() {
  Defaults d;
  const VramInfo v = queryVram();
  d.headroomGB = v.ok ? std::round(v.freeGB * 0.05 * 10.0) / 10.0 : 2.0;
  const double bf16Need =
      estimateVramNeed(false, kDefaultCheckW, kDefaultCheckH, kDefaultCheckFrames, false, kMps).gb;
  d.precision = (!kMps && v.ok && v.freeGB - d.headroomGB < bf16Need) ? 1 : 0;
  std::string src = v.ok ? "built-in (free " + fmtGB(v.freeGB) + " of " + fmtGB(v.totalGB) + " GB)"
                         : "built-in (GPU memory unavailable)";
  const UserConfig cfg = loadUserConfig();
  if (cfg.count("precision")) d.precision = cfg.at("precision") == "fp8" ? 1 : 0;
  if (cfg.count("headroom_gb")) d.headroomGB = std::atof(cfg.at("headroom_gb").c_str());
  if (cfg.count("unload")) d.unload = cfg.at("unload") == "1";
  if (cfg.count("precision") || cfg.count("headroom_gb") || cfg.count("unload"))
    src += " + user config";
  d.source = src;
  return d;
}

uint64_t hashImage(const OFX::Image& img) {
  const OfxRectI b = img.getBounds();
  const size_t rowBytes = size_t(b.x2 - b.x1) * 4 *
                          (img.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3);
  uint64_t h = 1469598103934665603ULL;
  for (int y = b.y1; y < b.y2; ++y) {
    const auto* row = static_cast<const unsigned char*>(img.getPixelAddress(b.x1, y));
    size_t i = 0;
    for (; i + 8 <= rowBytes; i += 8) {
      uint64_t w;
      std::memcpy(&w, row + i, 8);
      h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < rowBytes; ++i) h = (h ^ row[i]) * 1099511628211ULL;
  }
  return h;
}

std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

// OpenFX 1.5 colour properties through the raw property suite (the 1.4 Support library has no
// colour calls; same approach as Matte Refine ML, plugin_ml.cpp). A string property's first
// value, or "" when the host does not set it (a host without colour management).
std::string propString(OfxPropertySetHandle h, const char* prop) {
  char* s = nullptr;
  if (!h || OFX::Private::gPropSuite->propGetString(h, prop, 0, &s) != kOfxStatOK || !s) return "";
  return s;
}

HostInfo hostInfo() {
  HostInfo h;
  if (const auto* d = OFX::getImageEffectHostDescription()) {
    h.name = d->hostName;
    h.label = d->hostLabel;
    h.version = d->versionLabel + " (" + std::to_string(d->versionMajor) + "." +
                std::to_string(d->versionMinor) + "." + std::to_string(d->versionMicro) + ")";
    h.background = d->hostIsBackground;
  }
  return h;
}

// What the last Process decided about the ROI, saved with the setup (hidden param roiState).
// Render needs it for the cache key without scanning the clip (decision 63).
struct RoiState {
  bool valid = false;    // a state exists and matches the current ROI settings
  bool active = false;   // the source alpha was a matte: the model saw `box` only
  RoiBox box{0, 0, 0, 0};
  bool mask = false;     // Black Out Outside ROI was on
  std::string settings;  // roiSettings() at that time; a mismatch means "reprocess"
};

std::string formatRoiState(const RoiState& r) {
  char b[160];
  std::snprintf(b, sizeof b, "%d,%d,%d,%d,%d,%d;%s", int(r.active), r.box.x, r.box.y, r.box.w,
                r.box.h, int(r.mask), r.settings.c_str());
  return b;
}

RoiState parseRoiState(const std::string& v) {
  RoiState r;
  int a = 0, k = 0;
  char set[64] = {0};
  if (std::sscanf(v.c_str(), "%d,%d,%d,%d,%d,%d;%63s", &a, &r.box.x, &r.box.y, &r.box.w, &r.box.h,
                  &k, set) == 7) {
    r.valid = true;
    r.active = a != 0;
    r.mask = k != 0;
    r.settings = set;
  }
  return r;
}

// True if the image has an alpha channel with any value below 0.99 (a matte, not just opaque).
bool alphaVaries(const OFX::Image& img) {
  if (img.getPixelComponents() != OFX::ePixelComponentRGBA ||
      img.getPixelDepth() != OFX::eBitDepthFloat)
    return false;
  const OfxRectI b = img.getBounds();
  for (int y = b.y1; y < b.y2; ++y) {
    const auto* p = static_cast<const float*>(img.getPixelAddress(b.x1, y));
    for (int x = 0; x < b.x2 - b.x1; ++x)
      if (p[size_t(x) * 4 + 3] < 0.99f) return true;
  }
  return false;
}

int componentsOf(const OFX::Image& img) {
  switch (img.getPixelComponents()) {
    case OFX::ePixelComponentRGBA: return 4;
    case OFX::ePixelComponentRGB: return 3;
    default: return 1;
  }
}

std::atomic<unsigned> gJobSeq{0};

class AlphaGenEffect : public OFX::ImageEffect {
 public:
  explicit AlphaGenEffect(OfxImageEffectHandle handle) : ImageEffect(handle) {
    src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    dst_ = fetchClip(kOfxImageEffectOutputClipName);
    chunkSize_ = fetchChoiceParam(kPChunkSize);
    frameHold_ = fetchChoiceParam(kPFrameHold);
    merge_ = fetchChoiceParam(kPMerge);
    roiMargin_ = fetchIntParam(kPRoiMargin);
    roiMask_ = fetchBooleanParam(kPRoiMask);
    roiLimit_ = fetchBooleanParam(kPRoiLimit);
    roiState_ = fetchStringParam(kPRoiState);
    roiInfo_ = fetchStringParam(kPRoiInfo);
    colourMode_ = fetchChoiceParam(kPColourMode);
    legacyColour_ = fetchChoiceParam(kPColour);
    status_ = fetchStringParam(kPStatus);
    showMarker_ = fetchBooleanParam(kPShowMarker);
    precision_ = fetchChoiceParam(kPPrecision);
    unload_ = fetchBooleanParam(kPUnload);
    headroom_ = fetchDoubleParam(kPHeadroom);
    accept_ = fetchBooleanParam(kPAccept);
    tokenCaption_ = fetchStringParam(kPTokenCaption);
    token_ = fetchStringParam(kPToken);
    modelDir_ = fetchStringParam(kPModelDir);
    existingDir_ = fetchStringParam(kPExistingDir);
    setupStatus_ = fetchStringParam(kPSetupStatus);
    debugLog_ = fetchBooleanParam(kPDebugLog);
    cacheDir_ = fetchStringParam(kPCacheDir);
    cacheInfo_ = fetchStringParam(kPCacheInfo);
    simSeconds_ = fetchDoubleParam(kPSimSeconds);
    simulate_ = fetchChoiceParam(kPSimulate);
    simNeed_ = fetchDoubleParam(kPSimNeed);
    python_ = fetchStringParam(kPPython);
    enforceGate_ = fetchBooleanParam(kPEnforceGate);
    autoRefresh_ = fetchBooleanParam(kPAutoRefresh);
    useDummy_ = fetchBooleanParam(kPUseDummy);
    initialized_ = fetchBooleanParam(kPInitialized);
    nodeIdParam_ = fetchStringParam(kPNodeId);
    refreshTick_ = fetchIntParam(kPRefreshTick);

    try {
      nodeIdParam_->getValue(nodeId_);
      if (nodeId_.empty()) {
        nodeId_ = randomId();  // saved with the setup; copies of a node share it
        nodeIdParam_->setValue(nodeId_);
      }
      LogScope scope(nodeId_);
      bool init = false;
      initialized_->getValue(init);
      LOGI("createInstance %p: initialized=%d context=%d", static_cast<void*>(this), int(init),
           int(getContext()));
      if (!init) {
        // New node (decision 21): take the current user defaults; a node loaded from a
        // saved setup keeps its own values because it carries initialized=true.
        const Defaults d = computeDefaults();
        precision_->setValue(d.precision);
        headroom_->setValue(d.headroomGB);
        unload_->setValue(d.unload);
        // Input Colour: new nodes start on Auto. The param's default is Bypass so that a setup
        // saved before it existed loads as Bypass (the old default); see below for the others.
        colourMode_->setValue(kColourAuto);
        initialized_->setValue(true);
        LOGI("new node: defaults [%s] precision=%s headroom=%.1f unload=%d input colour=Auto",
             d.source.c_str(), kPrecisions[d.precision], d.headroomGB, int(d.unload));
      } else {
        // A setup saved with the old dropdown on a colour space (not Bypass): it wanted a
        // conversion, so it moves to Auto once; the old param is reset so this runs only once.
        int legacy = 0;
        legacyColour_->getValue(legacy);
        if (legacy > 0) {
          colourMode_->setValue(kColourAuto);
          legacyColour_->setValue(0);
          LOGI("input colour: saved setup had the old Input Colour '%s': now Auto",
               kLegacyColourSpaces[std::min(legacy, 5)]);
        }
      }
      // Per-user values are not stored in setups; mirror them from the user config.
      const UserConfig cfg = loadUserConfig();
      accept_->setValue(setupState().licenceAccepted);
      if (cfg.count("model_dir")) modelDir_->setValue(cfg.at("model_dir"));
      if (cfg.count("existing_models")) existingDir_->setValue(cfg.at("existing_models"));
      cacheDir_->setValue(cacheSettings(cfg).root);
      debugLog_->setValue(logLevel() == LogLevel::Debug);
      refreshSetupUi();
      refreshCacheInfo();
    } catch (const std::exception& e) {
      LOGE("createInstance: setting param values FAILED: %s [%s]", e.what(), typeid(e).name());
    }
  }

  ~AlphaGenEffect() override {
    LogScope scope(nodeId_);
    stopWatcher();
    stopSetupWatcher();
    LOGI("destroyInstance %p", static_cast<void*>(this));
  }

  void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override {
    // Decision 43: ask for a matte-only output. Flame ignores this (test 3: output stays RGBA),
    // so render() also handles RGBA (decision 48). Kept: other hosts may honour it.
    prefs.setClipComponents(*dst_, OFX::ePixelComponentAlpha);
    LOGD("getClipPreferences: output set to Alpha");
  }

  int colourMode() {
    int m = kColourAuto;
    colourMode_->getValue(m);
    return m == kColourBypass ? kColourBypass : kColourAuto;
  }

  // The colourspace the host delivers the Source clip in (OpenFX 1.5 kOfxImageClipPropColourspace,
  // ofxColour.h); "" from a host without colour management.
  std::string deliveredColourspace() {
    return propString(src_->getPropertySet().propSetHandle(), kOfxImageClipPropColourspace);
  }

  // Input Colour (called from the factory's entry after the Support library's GetClipPreferences,
  // whose status is st). Auto requests kAutoColourspace for the Source clip on the out args
  // (OfxImageClipPropPreferredColourspaces_Source, ofxColour.h kOfxImageClipPropPreferredColourspaces;
  // Flame 2027.2 honours it there and refuses it on the clip descriptor: Matte Refine ML,
  // PHASE0_RESULTS.md "Colour probe"). The host reads out args only on kOfxStatOK. Bypass: nothing.
  OfxStatus requestColour(OfxPropertySetHandle out, OfxStatus st) {
    LogScope scope(nodeId_);
    const int mode = colourMode();
    std::string req = "no request";
    if (mode == kColourAuto) {
      const std::string prop = std::string(kOfxImageClipPropPreferredColourspaces) + "_" +
                               kOfxImageEffectSimpleSourceClipName;
      const char* names[] = {kAutoColourspace};
      const OfxStatus s = OFX::Private::gPropSuite->propSetStringN(out, prop.c_str(), 1, names);
      req = std::string("requested '") + kAutoColourspace + "' on " + prop + ": " + OFX::mapStatusToString(s);
      if (st == kOfxStatReplyDefault) st = kOfxStatOK;
    }
    LOGI("colour: clip preferences, Input Colour %s, style '%s', %s; Source clip colourspace now '%s'",
         kColourModeNames[mode],
         getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, false).c_str(), req.c_str(),
         deliveredColourspace().c_str());
    return st;
  }

  // GetOutputColourspace (OpenFX 1.5, unknown to the 1.4 Support library): the output is the Source
  // picture with the overlay (decision 48). Auto: display-referred (we convert ACEScg to display before
  // the overlay, so the overlay looks the same on any clip); Bypass: the clip's colourspace is unknown
  // to us, so no picture and no overlay: the matte goes out on both outputs, tagged "Matte" (data). Without an answer Flame mis-tags the Auto picture (looks converted twice;
  // Flame test, 2026-10-27). "" = no answer (host default).
  std::string outputColourspace() {
    LogScope scope(nodeId_);
    if (!src_->isConnected()) return "";
    if (colourMode() == kColourBypass) {  // the image output carries the matte: a data colourspace
      const std::string style = getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, false);
      return style == kOfxImageEffectColourManagementOCIO ? "Matte" : "";  // "Matte": Flame's aces2.0 config
    }
    const std::string cs = kOutputDisplay;
    LOGD("colour: output colourspace %s", cs.c_str());
    return cs;
  }

  void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override {
    // Flame reports our own setValue() calls back synchronously as changedParam (reason =
    // plugin edit); reacting to those recursed until Flame crashed (Flame test 2). Only user
    // edits are handled, and never re-entrantly.
    if (args.reason != OFX::eChangeUserEdit || inChangedParam_) return;
    struct Guard {
      bool& b;
      explicit Guard(bool& b) : b(b) { b = true; }
      ~Guard() { b = false; }
    } guard(inChangedParam_);
    LogScope scope(nodeId_);
    LOGI("changedParam %s (user) time=%g", name.c_str(), args.time);
    try {
      handleParam(name, args.time);
    } catch (const std::exception& e) {
      LOGE("changedParam %s FAILED: %s [%s]", name.c_str(), e.what(), typeid(e).name());
      throw;
    }
    refreshSetupUi();
  }

  void render(const OFX::RenderArguments& args) override {
    LogScope scope(nodeId_);
    LOGD("render t=%g interactive=%d sequential=%d window=(%d,%d)-(%d,%d)", args.time,
         int(args.interactiveRenderStatus), int(args.sequentialRenderStatus),
         args.renderWindow.x1, args.renderWindow.y1, args.renderWindow.x2, args.renderWindow.y2);
    std::unique_ptr<OFX::Image> out(dst_->fetchImage(args.time));
    if (!out) {
      LOGE("render t=%g: host returned no output image", args.time);
      OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::vector<float> matte;
    std::string message, caption;
    bool passThrough = false;  // never processed: source + white matte (decision 71)
    bool failRender = false;   // processed, but no matte: loud marker, then fail the render
    const OfxRectI db = out->getBounds();
    OfxRectI sb = db;
    std::unique_ptr<OFX::Image> src;
    const bool setupRequired = gateClosed();
    if (gateWasClosed_ && !setupRequired) {  // Setup finished in the background since the last look
      LOGI("render: setup is now complete; enabling the node's params");
      refreshSetupUi();
    }
    gateWasClosed_ = setupRequired;

    if (setupRequired) {
      message = "Setup required: see the Licence and Models columns.";
      caption = "SETUP REQUIRED";
    } else {
      src.reset(src_->fetchImage(args.time));
      if (!src) {
        message = "No source image (is the input connected?).";
        caption = "NOT CONNECTED";
      } else if (src->getPixelDepth() != OFX::eBitDepthFloat) {
        message = "Only 32-bit float images are supported.";
        caption = "NOT SUPPORTED";
      } else {
        sb = src->getBounds();
        const int w = sb.x2 - sb.x1, h = sb.y2 - sb.y1;
        const std::string cs = deliveredColourspace();
        const std::string outCs = propString(dst_->getPropertySet().propSetHandle(), kOfxImageClipPropColourspace);
        if (!loggedDeliveredCs_ || cs != lastDeliveredCs_ || outCs != lastOutputCs_) {  // what Flame did
          LOGI("colour: render: Input Colour %s, Source delivered in '%s', output clip tagged '%s'",
               kColourModeNames[colourMode()], cs.c_str(), outCs.c_str());
          lastDeliveredCs_ = cs;
          lastOutputCs_ = outCs;
          loggedDeliveredCs_ = true;
        }
        const std::string key = matteKey(hashImage(*src), paramSignature(nullptr, src.get()));
        std::vector<uint16_t> half;
        if (readMatte(cacheRoot(), key, w, h, half)) {
          matte.resize(half.size());
          halfToFloat(half.data(), matte.data(), matte.size());
          applyRoiLimit(*src, matte, w, h);
        } else if (isPending(cacheRoot(), key)) {
          message = "Processing: this frame is not ready yet.";
          caption = "PROCESSING";
        } else if (!processedBefore()) {
          message = "Not processed: press Process.";
          caption = "NOT PROCESSED";
          passThrough = true;
        } else {
          // Decision 71: this node had mattes, so a miss must not pass silently through a
          // big setup (cache purged or swept, another machine, source or settings changed).
          failRender = true;
          const std::string status = lastJobStatus();
          const std::string state = jsonField(status, "state");
          if (state == "failed") {
            message = "Processing failed: " + jsonField(status, "message") +
                      ". Press Process to try again.";
            caption = "FAILED";
          } else if (state == "cancelled") {
            message = "Processing was cancelled. Press Process to try again.";
            caption = "CANCELLED";
          } else {
            message = "Matte missing: no cached matte for this frame (cache purged, another "
                      "machine, or the source or settings changed since Process). Press Process.";
            caption = "MATTE MISSING";
          }
        }
        LOGD("render t=%g key=%s %s", args.time, key.c_str(),
             matte.empty() ? caption.c_str() : "cache hit");
      }
    }

    const int w = sb.x2 - sb.x1, h = sb.y2 - sb.y1;  // source frame size (the matte's size)
    const bool realMatte = !matte.empty();  // before drawMarker() fills it with the marker
    if (passThrough) {
      matte.assign(size_t(w) * h, 1.0f);
    } else if (matte.empty()) {
      bool marker = true;
      showMarker_->getValue(marker);
      if (marker || failRender) drawMarker(matte, w, h, caption);
    }
    const bool loud = setupRequired || failRender;  // the marker on the image output too
    // Flame always gives an RGBA output and shows it as an image output plus a matte output
    // (decision 43 attempt failed). Decision 48: the alpha (Flame's matte output) carries the
    // matte or marker. Decision 58: the image is the source with the matte overlaid in magenta
    // at 30% (overlayPixel, marker.h); frames without a matte show the plain source, except
    // SETUP REQUIRED, which shows the marker on the image too (decision 59: a node viewed on its
    // image output must not look like a working pass-through), and a missing matte on a
    // processed node (decision 71). A 1-channel output gets the matte.
    const bool rgbFromSource = src && src->getPixelDepth() == OFX::eBitDepthFloat;
    const OfxRectI& win = args.renderWindow;
    const int comps = dst_->getPixelComponents() == OFX::ePixelComponentRGBA  ? 4
                      : dst_->getPixelComponents() == OFX::ePixelComponentRGB ? 3
                                                                               : 1;
    if (!loggedOutput_) {
      LOGI("render: output components %s (%d channel(s))",
           dst_->getPixelComponentsProperty().c_str(), comps);
      loggedOutput_ = true;
    }
    const bool bypassOut = colourMode() == kColourBypass;  // decision 76: no picture, no overlay
    const bool displayOut = colourMode() == kColourAuto && deliveredColourspace() == kAutoColourspace;
    for (int y = std::max(win.y1, db.y1); y < std::min(win.y2, db.y2); ++y) {
      auto* px = static_cast<float*>(out->getPixelAddress(std::max(win.x1, db.x1), y));
      const int row = (h - 1) - (y - sb.y1);  // OFX is bottom-up, the matte top-down
      for (int x = std::max(win.x1, db.x1); x < std::min(win.x2, db.x2); ++x) {
        const int col = x - sb.x1;
        const bool inside = row >= 0 && row < h && col >= 0 && col < w;
        const float v = (!matte.empty() && inside) ? matte[size_t(row) * w + col] : 0.0f;
        if (comps == 1) {
          *px++ = v;
          continue;
        }
        const float* sp = (rgbFromSource && inside)
                              ? static_cast<const float*>(src->getPixelAddress(x, y))
                              : nullptr;
        if (loud || bypassOut) {
          px[0] = px[1] = px[2] = v;  // the marker, grey on black; Bypass: the matte on both outputs
        } else {
          static const float kBlack[3] = {0.0f, 0.0f, 0.0f};
          float disp[3];
          if (sp && displayOut) {  // Auto: ACEScg to display first, then the overlay (decision 76)
            acescgToDisplay(sp, disp);
            sp = disp;
          }
          overlayPixel(sp ? sp : kBlack, realMatte ? v : 0.0f, px);
        }
        px += 3;
        if (comps == 4) *px++ = v;
      }
    }
    try {
      if (caption.empty())
        clearPersistentMessage();
      else
        setPersistentMessage(failRender ? OFX::Message::eMessageError : OFX::Message::eMessageWarning,
                             "", message);
    } catch (const std::exception& e) {
      LOGW("render: persistent message FAILED: %s", e.what());
    }
    if (failRender) {  // after filling the output, in case the host shows it anyway
      LOGD("render t=%g: %s, failing the render (decision 71)", args.time, caption.c_str());
      OFX::throwSuiteStatusException(kOfxStatFailed);
    }
  }

 private:
  // Multiply Result by ROI (decision 63): the cached matte times this frame's source alpha.
  // Render-time only, so it can be toggled without reprocessing.
  void applyRoiLimit(const OFX::Image& src, std::vector<float>& matte, int w, int h) {
    bool limit = false;
    roiLimit_->getValue(limit);
    if (!limit) return;
    const RoiState st = currentRoiState();
    if (!st.valid || !st.active) return;
    const OfxRectI b = src.getBounds();
    const int comps = componentsOf(src);
    std::vector<float> row(w);
    for (int r = 0; r < h; ++r) {
      roiRowAlpha(static_cast<const float*>(src.getPixelAddress(b.x1, b.y2 - 1 - r)), comps, w, row.data());
      float* m = matte.data() + size_t(r) * w;
      for (int x = 0; x < w; ++x) m[x] *= row[x];
    }
  }

  // Scans the source's alpha over the whole clip (main thread, like the staging fetch). If the
  // alpha is opaque in every frame there is no ROI and the model sees the whole frame; else the
  // crop box is the union of the matte, grown by the margin (roi.h).
  bool computeRoi(int first, int n, int W, int H, RoiState& out, std::string& err) {
    int margin = 64;
    roiMargin_->getValue(margin);
    roiMask_->getValue(out.mask);
    out.settings = roiSettings();
    out.valid = true;
    out.box = {0, 0, W, H};
    RoiScan scan;
    for (int f = 0; f < n; ++f) {
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        err = "Flame did not provide source frame " + std::to_string(first + f) + ".";
        return false;
      }
      if (img->getPixelDepth() != OFX::eBitDepthFloat) {
        err = "Only 32-bit float images are supported.";
        return false;
      }
      const OfxRectI b = img->getBounds();
      if (b.x2 - b.x1 != W || b.y2 - b.y1 != H) {
        err = "Source frames differ in size within the clip.";
        return false;
      }
      const int comps = componentsOf(*img);
      for (int row = 0; row < H; ++row)
        scan.addRow(static_cast<const float*>(img->getPixelAddress(b.x1, b.y2 - 1 - row)), comps, W, row);
    }
    if (!scan.alphaVaries) {
      LOGI("process: source alpha is opaque in all %d frames: no ROI, whole frame", n);
      return true;
    }
    if (!scan.alpha.any()) {
      err = std::string("The source alpha is empty (nothing above ") +
            std::to_string(kRoiThreshold).substr(0, 4) + " in any frame), so there is no ROI.";
      return false;
    }
    out.active = true;
    out.box = expandRoi(scan.alpha, margin, W, H);
    LOGI("process: ROI from source alpha: bounds x %d-%d y %d-%d (union of %d frames), margin %d -> crop "
         "x %d y %d %dx%d of %dx%d, black out outside %d",
         scan.alpha.x0, scan.alpha.x1, scan.alpha.y0, scan.alpha.y1, n, margin, out.box.x,
         out.box.y, out.box.w, out.box.h, W, H, int(out.mask));
    return true;
  }

  void handleParam(const std::string& name, double time) {
    if (name == kPProcess) {
      process(time);
    } else if (name == kPPurgeNode) {
      purgeNode();
    } else if (name == kPSaveDefault) {
      saveAsDefault();
    } else if (name == kPViewLicence) {
      std::string err;
      const bool ok = showTextFile("LTX-2.x Licence", licencePath(), cleanEnvironment(), err);
      if (ok)
        LOGI("viewLicence: window opened");
      else
        LOGE("viewLicence: FAILED %s", err.c_str());
    } else if (name == kPAccept) {
      bool v = false;
      accept_->getValue(v);
      UserConfig cfg = loadUserConfig();
      if (v) {
        cfg["licence_accepted"] = timeStr("%Y-%m-%d");
        cfg["licence_hash"] = licenceHash();
      } else {
        cfg.erase("licence_accepted");
        cfg.erase("licence_hash");
      }
      saveUserConfig(cfg);
      LOGI("licence %s (hash %s)", v ? "accepted" : "withdrawn", licenceHash().c_str());
    } else if (name == kPModelDir || name == kPCacheDir || name == kPExistingDir) {
      std::string v;
      (name == kPModelDir ? modelDir_ : name == kPCacheDir ? cacheDir_ : existingDir_)->getValue(v);
      v = trimSlashes(v);
      UserConfig cfg = loadUserConfig();
      cfg[name == kPModelDir ? "model_dir" : name == kPCacheDir ? "cache_dir" : "existing_models"] = v;
      saveUserConfig(cfg);
      LOGI("%s set to %s", name.c_str(), v.c_str());
      refreshCacheInfo();
    } else if (name == kPDebugLog) {
      bool v = false;
      debugLog_->getValue(v);
      UserConfig cfg = loadUserConfig();
      cfg["log_level"] = v ? "debug" : "info";
      saveUserConfig(cfg);
      setLogLevel(v ? LogLevel::Debug : LogLevel::Info);
      LOGI("debug logging %s", v ? "on" : "off");
    } else if (name == kPCollectLogs) {
      collectLogs();
    } else if (name == kPDownload) {
      download();
    } else if (name == kPPurgeAll) {
      const std::string root = cacheRoot();
      const uint64_t freed = purgeAll(root);
      LOGI("purgeAll %s: %.1f MB freed", root.c_str(), freed / 1048576.0);
      refreshCacheInfo();
    }
  }

  bool gateClosed() {
    bool enforce = true;
#if LTXAG_DEV_BUILD
    enforceGate_->getValue(enforce);
#endif
    return enforce && !setupState().ready();
  }

  // Disable everything outside Licence/Models/Cache while setup is incomplete (decision 38).
  void refreshSetupUi() {
    const SetupState st = setupState();
    const bool closed = gateClosed();
    for (const char* p : {kPColourMode, kPProcess, kPShowMarker, kPPurgeNode, kPPrecision, kPUnload,
                          kPHeadroom, kPSaveDefault, kPChunkSize, kPMerge,
                          kPRoiMargin, kPRoiMask, kPRoiLimit}) {
      try {
        getParam(p)->setEnabled(!closed);
      } catch (const std::exception& e) {
        LOGW("setEnabled(%s) FAILED: %s", p, e.what());
      }
    }
    // <= 28 characters: Flame cuts text at ~30 (test 2 screenshots).
    setIfChanged(setupStatus_, std::string("Licence: ") + (st.licenceAccepted ? "OK" : "NO") +
                                   " | Models: " + (st.modelsReady ? "OK" : "NO"));
  }

  // Avoids needless host notifications (each setValue is reported back as a change).
  void setIfChanged(OFX::StringParam* p, const std::string& v) {
    try {
      std::string cur;
      p->getValue(cur);
      if (cur != v) p->setValue(v);
    } catch (const std::exception& e) {
      LOGW("setValue FAILED: %s", e.what());
    }
  }

  std::string cacheRoot() { return cacheSettings(loadUserConfig()).root; }

  void refreshCacheInfo() {
    setIfChanged(cacheInfo_, fmtGB(cacheBytes(cacheRoot()) / 1073741824.0) + " GB used");
  }

  bool useDummy() {
    bool dummy = false;
#if LTXAG_DEV_BUILD
    useDummy_->getValue(dummy);
#endif
    return dummy;
  }

  int frameHoldIndex() {
    int idx = 0;
    frameHold_->getValue(idx);
    return idx;
  }

  int frameHold() {
    const int idx = frameHoldIndex();
    return kFrameHolds[std::min(std::max(idx, 0), 4)];  // 0 = Auto
  }

  // Decision 63: the ROI box and Black Out Outside ROI change the matte, so they are part of the
  // key (two nodes on one source with different ROIs must not share mattes). Without an ROI (an
  // opaque alpha) the key is the old one, so earlier caches stay valid. Chunk Size and Merge are
  // not in it: Process overwrites the whole clip anyway. With no saved state that matches the
  // current ROI settings, a frame whose alpha is a matte gets "roi=unprocessed" (never a hit).
  // `img` is the frame being keyed; the state is decided by the last Process.
  // Input Colour: Bypass keeps the old key ("cs=Bypass"); Auto keys on the colourspace the host
  // delivered ("cs=Auto:ACEScg", "cs=Auto:" without colour management), which decides the
  // worker's conversion.
  std::string colourKey() {
    return colourMode() == kColourBypass ? std::string("Bypass") : "Auto:" + deliveredColourspace();
  }

  std::string paramSignature(const RoiState* roiOverride = nullptr, const OFX::Image* img = nullptr) {
    int prec = 0;
    precision_->getValue(prec);
    std::string sig = std::string("v1|cs=") + colourKey() + "|prec=" + effectivePrecision(prec) +
                      "|model=" + (useDummy() ? kDummyVersion : kModelVersion);
    // Decisions 74/75; Off = the old key. Auto: one key part per threshold version.
    const int hold = frameHold();
    if (hold != 1) sig += "|hold=" + (hold == 0 ? std::string(kAutoHoldVersion) : std::to_string(hold));
    const RoiState st = roiOverride ? *roiOverride : currentRoiState();
    if (st.valid) {
      if (st.active)
        sig += "|roi=" + std::to_string(st.box.x) + "," + std::to_string(st.box.y) + "," +
               std::to_string(st.box.w) + "," + std::to_string(st.box.h) + ",k" +
               std::to_string(int(st.mask));
    } else if (img && alphaVaries(*img)) {
      sig += "|roi=unprocessed";
    }
    return sig;
  }

  std::string roiSettings() {
    int margin = 64;
    bool mask = false;
    roiMargin_->getValue(margin);
    roiMask_->getValue(mask);
    return "m" + std::to_string(margin) + "k" + std::to_string(int(mask));
  }

  // The saved ROI state, valid only while the ROI settings still match the ones it was made with.
  RoiState currentRoiState() {
    std::string v;
    roiState_->getValue(v);
    RoiState st = parseRoiState(v);
    if (st.valid && st.settings != roiSettings()) st.valid = false;
    return st;
  }

  // Decision 71: roiState doubles as the record that this node was processed. Every launched
  // Process writes it (since 0.1.3), it is saved with the setup and copied with the node, and
  // Purge Cache (node) clears it.
  bool processedBefore() {
    std::string v;
    roiState_->getValue(v);
    return !v.empty();
  }

  std::string lastJobStatus() {
    std::string id;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      id = lastJobId_;
    }
    return id.empty() ? "" : readFile(cacheRoot() + "/jobs/" + id + ".json");
  }

  std::string pythonPath() {
    std::string p;
    python_->getValue(p);
    return p.empty() ? "/usr/bin/python3" : p;
  }

  void error(const std::string& msg) {
    LOGW("error shown to user: %s", msg.c_str());
    sendMessage(OFX::Message::eMessageError, "", msg);
  }

  std::string workerSocket() { return homeDir() + "/.cache/ltx-alphagen/run/worker.sock"; }

  // Decisions 12/34(c)/56: never silently downgrade. Size-aware estimate from the measurements
  // (vram_estimate.h). A live resident worker's memory counts as available whatever its
  // precision: a matching one is reused, another one is restarted (which frees it). Flame test 1
  // (2026-10-11) showed two false refusals when this only counted a matching precision.
  // Decision 56 (user, 2026-10-11): too little memory is a warning with Yes/No, not a refusal,
  // because the estimate rests on few measurements. The numbers go into the manifest so job.py
  // can log estimate vs actual (logs/vram_stats.csv).
  // GPU memory held by a live resident worker (counts as available whatever its precision).
  double residentWorkerGB(unsigned& pid) {
    pid = 0;
    const std::string info = readFile(workerSocket() + ".json");
    if (info.empty() || !pidAlive(std::atoi(jsonField(info, "pid").c_str()))) return 0.0;
    pid = unsigned(std::atoi(jsonField(info, "pid").c_str()));
    return std::atof(jsonField(info, "vram_gb").c_str());
  }

  // Decision 61: Auto takes the longest chunk size whose estimated need fits the available
  // memory, else the smallest. Without NVML it takes Full. The overlap goes with the size.
  // With a Frame Hold (74) the model frames of a chunk are what counts.
  int chooseChunkSize(int mode, bool fp8, int w, int h, int frames, bool resident, double head, int hold) {
    if (mode > 0) return mode - 1;
    const VramInfo v = queryVram();
    if (!v.ok) return 0;
    unsigned pid = 0;
    const double available = v.freeGB + residentWorkerGB(pid) - head;
    for (int s = 0; s < kChunkSizes; ++s) {
      const double need =
          estimateVramNeed(fp8, w, h, heldModelFrames(std::min(heldChunkLen(s, hold), frames), hold), resident, kMps).gb;
      LOGI("chunk size auto: %d frames/chunk needs %.1f GB, available %.1f GB", kSizeLen[s], need, available);
      if (need <= available) return s;
    }
    return kChunkSizes - 1;
  }

  // modelFrames: of the largest chunk, holds included (74/75).
  bool preflight(int modelFrames, int w, int h, std::string& vramJson) {
    int prec = 0;
    double head = 0.0, simNeed = 0.0;
    bool unload = true;
    precision_->getValue(prec);
    headroom_->getValue(head);
    unload_->getValue(unload);
    simNeed_->getValue(simNeed);
    const VramNeed est = estimateVramNeed(prec == 1, w, h, modelFrames, !unload, kMps);
    const double need = simNeed > 0.0 ? simNeed : est.gb;
    const VramInfo v = queryVram();
    unsigned workerPid = 0;
    const double reusable = residentWorkerGB(workerPid);
    const double available = v.freeGB + reusable - head;
    LOGI("preflight: %s %dx%d largest chunk %d model frames -> model %dx%dx%d, need %.1f GB%s; free %.1f + resident "
         "%.1f - reserve %.1f = %.1f GB available (nvml %s)",
         effectivePrecision(prec).c_str(), w, h, modelFrames, est.modelW, est.modelH, est.modelFrames, need,
         simNeed > 0.0 ? " (simulated)" : "", v.freeGB, reusable, head, available, v.ok ? "ok" : "unavailable");
    bool warned = false;
    if (v.ok && need > available) {
      // Who holds the rest: Flame (this process), our resident worker, everyone else.
      double flame = 0.0, others = 0.0;
      std::string list;
      int listed = 0;
      for (const auto& p : queryGpuProcesses()) {
        const double gb = std::max(0.0, p.usedGB);
        if (p.pid == unsigned(getpid())) {
          flame += gb;
        } else if (p.pid != workerPid) {
          others += gb;
          if (gb >= 0.1 && listed < 4) {
            list += std::string(listed++ ? ", " : "") + p.name + " " + fmtGB(gb);
          }
        }
      }
      LOGW("preflight: GPU %.1f GB total, %.1f free; Flame holds %.1f GB, LTX worker %.1f GB, other apps %.1f GB (%s)",
           v.totalGB, v.freeGB, flame, reusable, others, list.c_str());
      std::string msg = "This clip may not fit in GPU memory.\n\nEstimated need: about " + fmtGB(need) +
                        " GB (" + effectivePrecision(prec) + ", " + std::to_string(est.modelW) + "x" +
                        std::to_string(est.modelH) + ", " + std::to_string(est.modelFrames) +
                        " frames per chunk).\nAvailable: " + fmtGB(available) + " GB = " + fmtGB(v.freeGB) +
                        " GB free";
      if (reusable > 0.0) msg += " + " + fmtGB(reusable) + " GB held by the LTX worker";
      msg += " - " + fmtGB(head) + " GB reserved for Flame.\nIn use on the " + fmtGB(v.totalGB) +
             " GB GPU: Flame " + fmtGB(flame) + " GB, other apps " + fmtGB(others) + " GB" +
             (list.empty() ? "" : " (" + list + ")") + ".\n\nThe estimate is conservative, so it may " +
             (kMps ? "still work; if not, macOS swaps memory to disk and the job (and the Mac) slows down "
                     "badly. A smaller Chunk Size needs less."  // Metal over-commit swaps (devices.py)
                   : "still work; if not, the job stops with an out-of-memory error and Flame is not affected.") +
             (!kMps && prec == 0 ? " fp8 needs about 17 GB less." : "") + "\n\nProcess anyway?";
      const OFX::Message::MessageReplyEnum reply = sendMessage(OFX::Message::eMessageQuestion, "", msg);
      LOGI("preflight: warning shown, reply %d (0 OK, 1 Yes, 2 No, 3 Failed)", int(reply));
      if (reply == OFX::Message::eMessageReplyNo) return false;
      if (reply != OFX::Message::eMessageReplyYes) {
        // The host did not ask (decision 56 fallback B): go ahead, but say so in the footer.
        setPersistentMessage(OFX::Message::eMessageWarning, "",
                             "Processing although the clip may not fit in GPU memory (need ~" + fmtGB(need) +
                                 " GB, available " + fmtGB(available) + " GB).");
      }
      warned = true;
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"estimate_gb\":%.2f,\"free_gb\":%.2f,\"total_gb\":%.2f,\"resident_gb\":%.2f,"
                  "\"reserve_gb\":%.2f,\"model_w\":%d,\"model_h\":%d,\"model_frames\":%d,\"nvml\":%s,"
                  "\"warned\":%s,",
                  need, v.freeGB, v.totalGB, reusable, head, est.modelW, est.modelH, est.modelFrames,
                  v.ok ? "true" : "false", warned ? "true" : "false");
    vramJson = std::string(buf) + "\"gpu\":" + jsonStr(v.name) + ",\"driver\":" + jsonStr(v.driver) + "}";
    return true;
  }

  // Fetches every frame of the clip during this action and hands them to a detached job.
  void process(double time) {
    if (gateClosed()) return error("Complete the Licence and Models setup first.");
    const UserConfig cfg = loadUserConfig();
    std::string modelDir;
    modelDir_->getValue(modelDir);
    modelDir = trimSlashes(modelDir);  // ".../models/" must not look like another folder (restarts the worker)
    const std::string ltxPython = cfg.count("ltx_python") ? cfg.at("ltx_python") : "";
    if (!useDummy()) {
      struct stat st {};
      if (ltxPython.empty() || stat(ltxPython.c_str(), &st) != 0)
        return error("The LTX runtime is not installed (no ltx_python in " + userConfigPath() + ").");
      if (stat((modelDir + "/diffusion_models").c_str(), &st) != 0)
        return error("No models found in the Model Folder: " + modelDir);
    }
    const OfxRangeD r = src_->getFrameRange();
    const int first = int(std::lround(r.min));
    const int n = int(std::lround(r.max)) - first + 1;
    const OfxRectD rod = src_->getRegionOfDefinition(time);
    const int fullW = int(std::lround(rod.x2 - rod.x1)), fullH = int(std::lround(rod.y2 - rod.y1));

    // Decision 63: the crop. Without an alpha matte the box is the whole frame.
    RoiState roi;
    std::string err;
    if (!computeRoi(first, n, fullW, fullH, roi, err)) return error(err);
    const bool useRoi = roi.active;
    // Decision 72: no 6K. Checked on what the model sees (the crop), before anything is staged.
    if (std::max(roi.box.w, roi.box.h) >= kMaxLongSide)
      return error(std::string(useRoi ? "The region of interest" : "The frame") + " is " +
                   std::to_string(roi.box.w) + "x" + std::to_string(roi.box.h) +
                   ": too large for the LTX model (6K and above are not supported). Scale the clip "
                   "down before this node and the matte up after it, or give the source an alpha "
                   "that limits the region of interest.");
    int prec = 0, chunkMode = 0, mergeMode = 0;
    bool unload = true;
    double head = 0.0;
    precision_->getValue(prec);
    if (kMps && prec == 1)  // decision 34: no silent downgrade
      return error("fp8 is not available on Apple Silicon (MPS has no fp8 type): set Precision to bf16.");
    unload_->getValue(unload);
    headroom_->getValue(head);
    chunkSize_->getValue(chunkMode);
    merge_->getValue(mergeMode);
    // Frame Hold (74/75). Auto plans its chunks after staging, from the motion measured while
    // staging; memory is checked before that against the worst case (4x), which bounds any plan.
    const int hold = frameHold();
    const int planHold = hold == 0 ? kAutoMaxHold : hold;
    const int sizeIdx = chooseChunkSize(chunkMode, prec == 1, roi.box.w, roi.box.h, n, !unload, head, planHold);
    const int overlap = hold == 0 ? kSizeOverlap[sizeIdx] : heldOverlap(sizeIdx, hold);
    std::vector<Chunk> chunks;
    int modelFrames = std::min(kSizeLen[sizeIdx], heldModelFrames(n, planHold));
    if (hold != 0) {
      chunks = planChunks(n, heldChunkLen(sizeIdx, hold), overlap);
      modelFrames = 0;
      for (auto& c : chunks) c.hold = hold, modelFrames = std::max(modelFrames, heldModelFrames(c.len, hold));
    }
    LOGI("process: chunk size %s -> %d model frames, overlap %d; frame hold %s; merge %s",
         kChunkSizeNames[chunkMode], kSizeLen[sizeIdx], overlap, kFrameHoldNames[std::min(4, std::max(0, frameHoldIndex()))],
         kMergeNames[mergeMode]);
    std::string vramJson;
    if (!preflight(modelFrames, roi.box.w, roi.box.h, vramJson)) return;
    const std::string sig = paramSignature(&roi);
    const std::string root = cacheRoot();
    const std::string jobId = std::to_string(getpid()) + "-" + std::to_string(gJobSeq++) + "-" +
                              std::to_string(std::time(nullptr));
    LogScope scope(nodeId_, jobId);
    const std::string input = stagingDir() + "/ltxag_job_" + jobId + "_in.bin";
    LOGI("process: t=%g clip [%d,%d] %d frames, %s chunk(s), params %s", time, first, first + n - 1, n,
         hold == 0 ? "auto" : std::to_string(chunks.size()).c_str(), sig.c_str());

    std::vector<std::string> keys(n);
    int w = 0, h = 0;
    const int cw = roi.box.w, ch = roi.box.h;  // what the model sees
    FrameWriter writer;
    std::vector<float> rgb, maskRow;
    std::vector<uint16_t> frame;
    std::unique_ptr<MotionAnalyzer> analyzer;  // Auto Frame Hold (75)
    if (hold == 0) analyzer.reset(new MotionAnalyzer(cw, ch));
    std::vector<double> motion(n, 0.0);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < n; ++f) {
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        writer.close();
        unlink(input.c_str());
        return error("Flame did not provide source frame " + std::to_string(first + f) + ".");
      }
      const OfxRectI b = img->getBounds();
      if (f == 0) {
        w = b.x2 - b.x1, h = b.y2 - b.y1;
        LOGI("process: source %dx%d %s depth %d", w, h, img->getPixelComponentsProperty().c_str(),
             int(img->getPixelDepth()));
        if (img->getPixelDepth() != OFX::eBitDepthFloat)
          return error("Only 32-bit float images are supported.");
        if (w != fullW || h != fullH)
          return error("Source frame size differs from the clip's region of definition.");
        if (!writer.open(input, cw, ch, n, err)) {
          unlink(input.c_str());
          return error(err);
        }
        rgb.resize(size_t(cw) * 3);
        maskRow.resize(size_t(w));
        frame.resize(size_t(cw) * ch * 3);
      } else if (b.x2 - b.x1 != w || b.y2 - b.y1 != h) {
        writer.close();
        unlink(input.c_str());
        return error("Source frames differ in size within the clip.");
      }
      const int sc = img->getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3;
      // Black Out Outside ROI: the source times its own alpha matte, in the source's colour space
      // (black stays near black after the colour conversion).
      const bool blackOut = useRoi && roi.mask;
      for (int row = 0; row < ch; ++row) {
        const int srcRow = roi.box.y + row;
        const auto* p = static_cast<const float*>(img->getPixelAddress(b.x1 + roi.box.x, b.y2 - 1 - srcRow));
        for (int x = 0; x < cw; ++x)
          for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] = p[size_t(x) * sc + k];
        if (useRoi && (blackOut || analyzer))
          roiRowAlpha(static_cast<const float*>(img->getPixelAddress(b.x1, b.y2 - 1 - srcRow)), sc, w,
                      maskRow.data());
        // Motion on the picture before Black Out, inside the ROI matte (75).
        if (analyzer) analyzer->addRow(row, rgb.data(), useRoi ? maskRow.data() + roi.box.x : nullptr);
        if (blackOut) {
          for (int x = 0; x < cw; ++x)
            for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] *= maskRow[size_t(roi.box.x + x)];
        }
        floatToHalf(rgb.data(), frame.data() + size_t(row) * cw * 3, rgb.size());
      }
      if (analyzer) motion[f] = analyzer->endFrame();
      keys[f] = matteKey(hashImage(*img), sig);
      if (!writer.writeFrame(frame.data(), err)) {
        writer.close();
        unlink(input.c_str());
        return error(err);
      }
    }
    writer.close();
    const double sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("process: staged %d frames in %.2f s (%.1f ms/frame), %.1f MB in %s", n, sec,
         1000.0 * sec / n, double(n) * cw * ch * 6 / 1048576.0, input.c_str());
    if (hold == 0) {
      chunks = planAutoChunks(autoHolds(motion), sizeIdx);
      std::vector<double> sorted(motion.begin() + 1, motion.end());
      std::sort(sorted.begin(), sorted.end());
      std::string plan;
      const int total = planModelFrames(chunks);
      for (const auto& c : chunks)
        plan += (plan.empty() ? "" : " ") + std::to_string(c.start) + "+" + std::to_string(c.len) + "@" +
                std::to_string(c.hold) + "x";
      LOGI("process: auto hold: motion px/frame median %.1f, max %.1f; %d chunk(s) %s; %d model frames "
           "for %d frames",
           sorted.empty() ? 0.0 : sorted[sorted.size() / 2], sorted.empty() ? 0.0 : sorted.back(),
           int(chunks.size()), plan.c_str(), total, n);
      std::string per;
      for (int f = 1; f < n; ++f) per += (f > 1 ? " " : "") + std::to_string(int(std::lround(motion[f])));
      LOGD("process: auto hold: motion per frame %s", per.c_str());
    }

    double seconds = 10.0;
    int sim = 0;
    simSeconds_->getValue(seconds);
    simulate_->getValue(sim);
    std::ostringstream js;
    js << "{\"job_id\":" << jsonStr(jobId) << ",\"node_id\":" << jsonStr(nodeId_)
       << ",\"input\":" << jsonStr(input) << ",\"width\":" << cw << ",\"height\":" << ch
       << ",\"full_width\":" << w << ",\"full_height\":" << h;
    if (useRoi)
      js << ",\"roi\":[" << roi.box.x << "," << roi.box.y << "," << roi.box.w << "," << roi.box.h << "]";
    js << ",\"merge\":" << (mergeMode == 1 ? "\"flow\"" : "\"smooth\"")
       << ",\"frames\":" << n << ",\"chunks\":[";
    for (size_t c = 0; c < chunks.size(); ++c)
      js << (c ? "," : "") << "[" << chunks[c].start << "," << chunks[c].len << "]";
    js << "],\"holds\":[";
    for (size_t c = 0; c < chunks.size(); ++c) js << (c ? "," : "") << chunks[c].hold;
    js << "],\"frame_keys\":[";
    for (int f = 0; f < n; ++f) js << (f ? "," : "") << jsonStr(keys[f]);
    js << "],\"contrib\":[";
    for (int f = 0; f < n; ++f) {
      js << (f ? "," : "") << "[";
      const auto contrib = frameContributions(chunks, f, overlap, mergeMode == 0);
      for (size_t c = 0; c < contrib.size(); ++c)
        js << (c ? "," : "") << "[" << contrib[c].chunk << "," << contrib[c].weight << "]";
      js << "]";
    }
    js << "],\"cache_root\":" << jsonStr(root)
       << ",\"worker\":"
       << jsonStr(resourcesDir() + (useDummy() ? "/worker/dummy_worker.py" : "/worker/ltx_worker.py"));
    // Input Colour: the mode and the colourspace the host delivered the frames in (worker:
    // colour.Converter). ocio_config stays "": the worker uses Flame's ACES 2.0 config, which has
    // the fixed view; the host's config is only logged.
    const std::string deliveredCs = deliveredColourspace();
    LOGI("process: Input Colour %s, Source delivered in '%s' (host OCIO config '%s')",
         kColourModeNames[colourMode()], deliveredCs.c_str(),
         getPropertySet().propGetString(kOfxImageEffectPropOCIOConfig, false).c_str());
    js << ",\"colour\":" << jsonStr(kColourModeNames[colourMode()]) << ",\"colour_tag\":" << jsonStr(deliveredCs);
    if (!useDummy()) {
      js << ",\"worker_python\":" << jsonStr(ltxPython) << ",\"worker_args\":[\"--model-dir\","
         << jsonStr(modelDir) << ",\"--embeddings\"," << jsonStr(root + "/prompt-empty.safetensors")
         << "],\"worker_socket\":" << jsonStr(workerSocket()) << ",\"model_dir\":" << jsonStr(modelDir)
         << ",\"unload\":" << (unload ? "true" : "false") << ",\"ocio_config\":\"\",\"seed\":1234";
    }
    js
       << ",\"seconds\":" << seconds << ",\"simulate\":" << jsonStr(kSimulations[sim])
       << ",\"precision\":" << jsonStr(effectivePrecision(prec)) << ",\"vram\":" << vramJson
       << ",\"flame_pid\":" << getpid()
       << ",\"title\":" << jsonStr("LTX Alpha Gen - " + std::to_string(n) + " frames") << "}";
    const std::string manifest = stagingDir() + "/ltxag_job_" + jobId + ".json";
    {
      std::ofstream mf(manifest);
      mf << js.str();
    }
    const std::string level = logLevel() == LogLevel::Debug ? "debug" : "info";
    // Before the launch: the render Flame triggers right after this button press must already
    // see PROCESSING (test 2: it raced job.py's own markers and showed NOT PROCESSED).
    setIfChanged(roiState_, formatRoiState(roi));  // render keys on it from now on
    setIfChanged(roiInfo_, useRoi ? "Crop " + std::to_string(roi.box.w) + "x" + std::to_string(roi.box.h)
                                  : std::string("None (alpha opaque)"));
    markPending(root, keys, getpid());
    // job.py needs numpy: the system python3 has it on Rocky 9 but not on macOS, where the job
    // runs under the LTX runtime's own Python (decision 65; Setup is stdlib-only and keeps
    // pythonPath()).
#ifdef __APPLE__
    const std::string jobPython = ltxPython.empty() ? pythonPath() : ltxPython;
#else
    const std::string jobPython = pythonPath();
#endif
    const bool ok = launchDetached(
        {jobPython, "-I", "-B", resourcesDir() + "/worker/job.py", manifest},
        cleanEnvironment({"LTXAG_LOG_LEVEL=" + level}), err);
    if (!ok) {
      clearPending(root, keys);
      unlink(input.c_str());
      unlink(manifest.c_str());
      return error("Could not start processing: " + err);
    }
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      lastJobId_ = jobId;
    }
    setIfChanged(status_, "Processing " + std::to_string(n) + " frames");
    LOGI("process: job launched (job python %s, worker %s)", jobPython.c_str(),
         useDummy() ? "dummy" : ltxPython.c_str());
    startWatcher(jobId, root, keys);
  }

  // --- Decision 42 (a): follow the job and nudge Flame to re-render. EXPERIMENT. ---
  void startWatcher(const std::string& jobId, const std::string& root, const std::vector<std::string>& keys) {
    stopWatcher();
    bool enabled = true;
    autoRefresh_->getValue(enabled);
    if (!enabled) {
      LOGI("auto refresh off: no watcher");
      return;
    }
    stopFlag_ = false;
    const std::string node = nodeId_;
    watcher_ = std::thread([this, jobId, root, node, keys] { watch(jobId, root, node, keys); });
  }

  void stopWatcher() {
    stopFlag_ = true;
    if (watcher_.joinable()) watcher_.join();
  }

  // Setup runs as its own process; nothing tells the node when it is done, so the status stayed
  // "Models: NO" and the params disabled until the next user edit (Mac test 2026-10-13). This
  // thread polls the user config until Setup reports ready, then refreshes the Setup UI; changes
  // from a non-action thread show at the next interaction in Flame (see "Flame host behaviour").
  void startSetupWatcher() {
    stopSetupWatcher();
    setupStop_ = false;
    const std::string node = nodeId_;
    setupWatcher_ = std::thread([this, node] {
      LogScope scope(node);
      LOGI("setup watcher started (this thread is not an OFX action thread)");
      const auto t0 = std::chrono::steady_clock::now();
      while (!setupStop_ && std::chrono::steady_clock::now() - t0 < std::chrono::hours(4)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!setupState().ready()) continue;
        try {
          refreshSetupUi();
          LOGI("setup watcher: setup complete; Setup UI refreshed from a non-action thread");
        } catch (const std::exception& e) {
          LOGE("setup watcher: refresh FAILED: %s [%s]", e.what(), typeid(e).name());
        }
        return;
      }
      LOGI("setup watcher finished without a ready setup");
    });
  }

  void stopSetupWatcher() {
    setupStop_ = true;
    if (setupWatcher_.joinable()) setupWatcher_.join();
  }

  void watch(const std::string& jobId, const std::string& root, const std::string& node,
             const std::vector<std::string>& keys) {
    LogScope scope(node, jobId);
    LOGI("watcher started (this thread is not an OFX action thread)");
    std::string lastSeen;
    int tick = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (!stopFlag_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const std::string j = readFile(root + "/jobs/" + jobId + ".json");
      if (j.empty()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
          // job.py never ran, so the markers written before the launch (with Flame's pid) would
          // otherwise show PROCESSING for as long as Flame runs.
          clearPending(root, keys);
          LOGW("watcher: job wrote no status within 60 s; cleared its pending markers, giving up");
          return;
        }
        continue;
      }
      const std::string state = jsonField(j, "state");
      const std::string seen = state + "/" + jsonField(j, "chunks_done");
      if (seen == lastSeen) continue;
      lastSeen = seen;
      std::string text;
      if (state == "running")
        text = "Processing: " + jsonField(j, "chunks_done") + "/" + jsonField(j, "chunks") +
               " chunks done";
      else if (state == "done")
        text = "Done: " + jsonField(j, "frames_done") + " frames";
      else if (state == "cancelled")
        text = "Cancelled";
      else
        text = "Failed: " + jsonField(j, "message").substr(0, 20);
      try {
        LOGI("watcher: %s -> status + refresh tick %d from a non-action thread", seen.c_str(),
             tick + 1);
        setIfChanged(status_, text);
        refreshTick_->setValue(++tick);
        LOGI("watcher: host accepted the param changes");
      } catch (const std::exception& e) {
        LOGE("watcher: param change FAILED: %s [%s]", e.what(), typeid(e).name());
        return;
      }
      if (state != "running") break;
    }
    LOGI("watcher finished");
  }

  void purgeNode() {
    const OfxRangeD r = src_->getFrameRange();
    const std::string sig = paramSignature();
    std::vector<std::string> keys;
    for (int t = int(std::lround(r.min)); t <= int(std::lround(r.max)); ++t) {
      std::unique_ptr<OFX::Image> img(src_->fetchImage(double(t)));
      if (img) keys.push_back(matteKey(hashImage(*img), sig));
    }
    const size_t n = purgeKeys(cacheRoot(), keys);
    LOGI("purgeNode: %zu of %zu frames had cached mattes, removed", n, keys.size());
    setIfChanged(roiState_, "");  // back to never processed: pass-through (decision 71)
    setIfChanged(status_, "Cache purged: " + std::to_string(n) + " frames");
    refreshCacheInfo();
  }

  void saveAsDefault() {
    int prec = 0;
    double head = 0.0;
    bool unl = true;
    precision_->getValue(prec);
    headroom_->getValue(head);
    unload_->getValue(unl);
    UserConfig cfg = loadUserConfig();
    cfg["precision"] = kPrecisions[prec];
    cfg["headroom_gb"] = fmtGB(head);
    cfg["unload"] = unl ? "1" : "0";
    const bool ok = saveUserConfig(cfg);
    LOGI("saveAsDefault: %s precision=%s headroom=%.1f unload=%d", ok ? "ok" : "FAILED",
         kPrecisions[prec], head, int(unl));
    sendMessage(OFX::Message::eMessageMessage, "",
                ok ? "Saved as your default for new LTX Alpha Gen nodes."
                   : "Could not write " + userConfigPath());
  }

  void download() {
    std::string token, dir, existing;
    token_->getValue(token);
    modelDir_->getValue(dir);
    existingDir_->getValue(existing);
    dir = trimSlashes(dir);
    existing = trimSlashes(existing);
    LOGI("download pressed: licence=%d token length=%zu model dir=%s existing=%s",
         int(setupState().licenceAccepted), token.size(), dir.c_str(), existing.c_str());
    if (!setupState().licenceAccepted)
      return error("Accept the LTX-2 Community License before downloading.");
    // The token is optional: everything may already be in the Model Folder or the Existing
    // Models folder (decision 51). The helper says which files still need it.
    std::string err;
    const std::string helper = resourcesDir() + "/helper/setup_helper.py";
    std::vector<std::string> argv = {pythonPath(), "-I", "-B", helper, "--model-dir", dir,
                                     "--cache-root", cacheRoot()};
    if (!existing.empty()) {
      argv.insert(argv.end(), {"--existing", existing});
      // Decision 66: link to the existing files instead of copying them. Asked each time; a host
      // that doesn't answer (neither Yes nor No) gets the copy, as before.
      const OFX::Message::MessageReplyEnum reply = sendMessage(
          OFX::Message::eMessageQuestion, "",
          "Link to the model files found in Existing Models instead of copying them?\n\n"
          "Yes: link (saves up to 73 GB). The Existing Models folder must then stay where it is "
          "and stay available; on a network folder every model load is slower.\n\n"
          "No: copy them into the Model Folder (independent of the original files).\n\n"
          "Either way each file is checked against its expected checksum first.");
      const bool link = reply == OFX::Message::eMessageReplyYes;
      LOGI("download: link instead of copy? reply %d -> %s", int(reply), link ? "link" : "copy");
      if (link) argv.push_back("--link");
    }
    const std::string level = logLevel() == LogLevel::Debug ? "debug" : "info";
    Env extras = {"LTXAG_LOG_LEVEL=" + level};
    if (!token.empty()) extras.push_back("HF_TOKEN=" + token);
    const bool ok = launchDetached(argv, cleanEnvironment(extras), err);
    token.assign(token.size(), '\0');
    token_->setValue("");  // decision 13: used once, then discarded
    // Decision 41: say what happened to the token instead of leaving a blank field.
    setIfChanged(tokenCaption_, ok && !token.empty() ? "Token used " + timeStr("%H:%M") + ", not stored"
                                                     : std::string(kTokenCaptionDefault));
    if (!ok) error("Download failed to start: " + err);
    LOGI("download: helper %s %s; token cleared", ok ? "launched" : "FAILED", err.c_str());
    if (ok) startSetupWatcher();
  }

  // Bundles logs, environment and job status files for a support request (decision 39).
  void collectLogs() {
    const std::string dir = logDir();
    {
      std::ofstream envf(dir + "/environment.txt");
      for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir())) envf << line << "\n";
    }
    const std::string bundle =
        homeDir() + "/ltx-alphagen-support-" + timeStr("%Y%m%d-%H%M%S") + ".tar.gz";
    std::vector<std::string> argv = {"/usr/bin/tar", "-czf", bundle, "-C",
                                     dir.substr(0, dir.rfind('/')), "logs"};
    const std::string root = cacheRoot();
    struct stat st {};
    if (stat((root + "/jobs").c_str(), &st) == 0) {
      argv.insert(argv.end(), {"-C", root, "jobs"});
    }
    std::string err;
    WorkerProcess tar;
    std::vector<std::string> lines;
    const bool started = tar.start(argv, cleanEnvironment(), err);
    if (started)
      while (tar.readLines(200, lines)) {
      }
    const int status = started ? tar.wait() : -1;
    const bool ok = started && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (const auto& l : lines) LOGW("collectLogs: tar: %s", l.c_str());
    LOGI("collectLogs: %s -> %s %s", ok ? "ok" : "FAILED", bundle.c_str(), err.c_str());
    sendMessage(ok ? OFX::Message::eMessageMessage : OFX::Message::eMessageError, "",
                ok ? "Support bundle written to " + bundle +
                         ". Please review it before sending: it contains your logs, system "
                         "details and folder paths, but no images and no token."
                   : "Could not write the support bundle: " + err);
  }

  OFX::Clip* src_;
  OFX::Clip* dst_;
  OFX::ChoiceParam* chunkSize_;
  OFX::ChoiceParam* frameHold_;
  OFX::ChoiceParam* merge_;
  OFX::IntParam* roiMargin_;
  OFX::BooleanParam* roiMask_;
  OFX::BooleanParam* roiLimit_;
  OFX::StringParam* roiState_;
  OFX::StringParam* roiInfo_;
  OFX::ChoiceParam* colourMode_;
  OFX::ChoiceParam* legacyColour_;
  OFX::StringParam* status_;
  OFX::BooleanParam* showMarker_;
  OFX::ChoiceParam* precision_;
  OFX::BooleanParam* unload_;
  OFX::DoubleParam* headroom_;
  OFX::BooleanParam* accept_;
  OFX::StringParam* tokenCaption_;
  OFX::StringParam* token_;
  OFX::StringParam* modelDir_;
  OFX::StringParam* existingDir_;
  OFX::StringParam* setupStatus_;
  OFX::BooleanParam* debugLog_;
  OFX::StringParam* cacheDir_;
  OFX::StringParam* cacheInfo_;
  OFX::DoubleParam* simSeconds_;
  OFX::ChoiceParam* simulate_;
  OFX::DoubleParam* simNeed_;
  OFX::StringParam* python_;
  OFX::BooleanParam* enforceGate_;
  OFX::BooleanParam* autoRefresh_;
  OFX::BooleanParam* useDummy_;
  OFX::BooleanParam* initialized_;
  OFX::StringParam* nodeIdParam_;
  OFX::IntParam* refreshTick_;
  std::string nodeId_;
  std::mutex jobMutex_;
  std::string lastJobId_;
  bool inChangedParam_ = false;
  bool loggedOutput_ = false;
  bool loggedDeliveredCs_ = false;
  std::string lastOutputCs_;
  std::string lastDeliveredCs_;  // render logs the delivered colourspace when it changes
  std::thread watcher_;
  std::thread setupWatcher_;
  std::atomic<bool> setupStop_{false};
  bool gateWasClosed_ = false;  // render: refresh the Setup UI once Setup has completed
  std::atomic<bool> stopFlag_{false};
};

template <class D>
D* labelled(D* p, const char* label, const char* hint) {
  p->setLabels(label, label, label);
  if (hint && *hint) p->setHint(hint);
  return p;
}

class AlphaGenFactory : public OFX::PluginFactoryHelper<AlphaGenFactory> {
 public:
  AlphaGenFactory()
      : PluginFactoryHelper<AlphaGenFactory>(kPluginId, LTXAG_VERSION_MAJOR, LTXAG_VERSION_MINOR) {}

  // Input Colour Auto: after the Support library's GetClipPreferences, the OpenFX 1.5 colour
  // request on the out args (the 1.4 Support library has no such call; as Matte Refine ML).
  static OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in,
                         OfxPropertySetHandle out) {
    if (std::strcmp(action, kOfxImageEffectActionGetOutputColourspace) == 0) {
      try {
        auto* fx = dynamic_cast<AlphaGenEffect*>(
            OFX::Private::retrieveImageEffectPointer((OfxImageEffectHandle)handle));
        const std::string cs = fx ? fx->outputColourspace() : "";
        if (cs.empty()) return kOfxStatReplyDefault;
        OFX::PropertySet(out).propSetString(kOfxImageClipPropColourspace, cs);
        return kOfxStatOK;
      } catch (const std::exception& e) {
        LOGE("colour: GetOutputColourspace FAILED: %s", e.what());
        return kOfxStatFailed;
      }
    }
    const OfxStatus st = FactoryMainEntryHelper<AlphaGenFactory>::mainEntry(action, handle, in, out);
    if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) != 0) return st;
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) return st;
    try {
      auto* fx = dynamic_cast<AlphaGenEffect*>(
          OFX::Private::retrieveImageEffectPointer((OfxImageEffectHandle)handle));
      return fx ? fx->requestColour(out, st) : st;
    } catch (const std::exception& e) {
      LOGE("colour: clip preferences FAILED: %s", e.what());
      return st;
    }
  }
  OfxPluginEntryPoint* getMainEntry() override { return entry; }

  void describe(OFX::ImageEffectDescriptor& desc) override {
    logConfigure();
    rotateLogs();
    LOGI("==== plugin load (describe) ====");
    for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir()))
      LOGI("env %s", line.c_str());
    const size_t staged = sweepStaleStaging();
    const SweepResult sw = sweepCache(cacheSettings(loadUserConfig()));
    LOGI("sweep: %zu stale staging file(s); cache aged %zu, capped %zu, kept %zu (%.1f MB), "
         "freed %.1f MB",
         staged, sw.aged, sw.capped, sw.kept, sw.bytesKept / 1048576.0, sw.bytesFreed / 1048576.0);
    desc.setLabels(kPluginLabel, kPluginLabel, kPluginLabel);
    desc.setPluginGrouping("LTX");
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
    // Input Colour Auto: OpenFX 1.5 colour management (ofxColour.h), as Matte Refine ML declares
    // it; a host without it ignores these, and the source arrives unconverted (no tag).
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementStyle,
                                        kOfxImageEffectColourManagementOCIO, false);
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementAvailableConfigs,
                                        "ofx-native-v1.5_aces-v1.3_ocio-v2.3", false);
  }

  void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum ctx) override {
    try {
      describeParams(desc);
      LOGI("describeInContext ctx=%d done", int(ctx));
    } catch (const std::exception& e) {
      LOGE("describeInContext ctx=%d FAILED: %s [%s]", int(ctx), e.what(), typeid(e).name());
      throw;
    }
  }

  void describeParams(OFX::ImageEffectDescriptor& desc) {
    const Defaults d = computeDefaults();
    LOGI("describeInContext: defaults [%s]", d.source.c_str());

    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setTemporalClipAccess(true);
    src->setSupportsTiles(false);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentAlpha);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);
    // Flame (test 2/3 screenshots): params flow into columns of <= 5 rows, 6 columns per
    // tab; each group starts a column; tabs are named after OFX pages in order. Earlier
    // builds listed every param as a page child and Flame ignored page membership. Here
    // pages list ONLY their groups (allowed by the OFX spec) — Flame still ignored that
    // (2026-10-10). Groups are defined in tab order; tabs come only from overflow, hence the
    // spacer columns below (decision 52).
    OFX::PageParamDescriptor* main = desc.definePageParam("Alpha Gen");
    OFX::PageParamDescriptor* setupPage = desc.definePageParam("Setup");
    OFX::PageParamDescriptor* proto = LTXAG_DEV_BUILD ? desc.definePageParam("Prototype") : nullptr;
    auto group = [&](const char* name, const char* text, OFX::PageParamDescriptor* page) {
      auto* g = desc.defineGroupParam(name);
      g->setLabels(text, text, text);
      g->setOpen(true);
      if (page) page->addChild(*g);
      return g;
    };
    auto place = [](OFX::ParamDescriptor& p, OFX::GroupParamDescriptor* g) { p.setParent(*g); };
    auto label = [&](const char* name, const char* text, const char* def) {
      auto* p = labelled(desc.defineStringParam(name), text, "");
      p->setStringType(OFX::eStringTypeLabel);
      p->setDefault(def);
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      return p;
    };
    auto button = [&](const char* name, const char* text, const char* hint) {
      return labelled(desc.definePushButtonParam(name), text, hint);
    };
    auto perUser = [](OFX::ValueParamDescriptor* p) {
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      p->setAnimates(false);
    };

    // --- Alpha Gen (5 rows) ---
    auto* gMain = group("grpMain", "Alpha Gen", main);
    auto* colour = labelled(desc.defineChoiceParam(kPColourMode), "Input Colour",
                            "Auto: Flame converts the source to ACEScg using the clip's colour "
                            "tag, then the model sees it through the ACES 2.0 SDR 100 nits (Rec.709) "
                            "view; Rec.709 video is used as it is. The image output then shows the "
                            "picture with the matte overlay in display space. Bypass: no conversion "
                            "(the source is already display-referred); the image output shows the "
                            "matte too.");
    for (const char* c : kColourModeNames) colour->appendOption(c);
    // Bypass, so setups saved before this param existed load as Bypass (the old default); new
    // nodes are set to Auto in createInstance.
    colour->setDefault(kColourBypass);
    desc.addClipPreferencesSlaveParam(*colour);  // a change re-runs GetClipPreferences
    place(*colour, gMain);
    place(*button(kPProcess, "Process",
                  "Generate mattes for the whole clip in the background. Progress and Cancel "
                  "appear in a separate window."),
          gMain);
    place(*label(kPStatus, "Status", "Not processed"), gMain);
    auto* marker = labelled(desc.defineBooleanParam(kPShowMarker), "Show Marker",
                            "Draw a faint marker (PROCESSING, NOT CONNECTED) on frames without a "
                            "matte. Off: such frames are plain black. A missing matte on a "
                            "processed node always shows MATTE MISSING.");
    marker->setDefault(true);
    place(*marker, gMain);

    // --- Performance (5 rows) ---
    auto* gPerf = group("grpPerf", "Performance", main);
    auto* prec = labelled(desc.defineChoiceParam(kPPrecision), "Precision",
                          "bf16: reference quality. fp8: about half the GPU memory.");
    for (const char* p : kPrecisions) prec->appendOption(p);
    prec->setDefault(d.precision);
    place(*prec, gPerf);
    auto* unload = labelled(desc.defineBooleanParam(kPUnload), "Unload GPU",
                            "Return all GPU memory to Flame when this node's processing ends. Turn "
                            "off on all but the last Alpha Gen node to reuse the loaded model.");
    unload->setDefault(d.unload);
    place(*unload, gPerf);
    auto* head = labelled(desc.defineDoubleParam(kPHeadroom), "VRAM Reserve",
                          "GPU memory (GB) kept free for Flame while processing.");
    head->setRange(0.0, 256.0);
    head->setDisplayRange(0.0, 32.0);
    head->setDefault(d.headroomGB);
    place(*head, gPerf);
    place(*button(kPSaveDefault, "Save Default",
                  "Use this column's values for every new LTX Alpha Gen node."),
          gPerf);

    // --- Chunking (4 rows; decisions 61, 62) ---
    auto* gChunk = group("grpChunk", "Chunking", main);
    auto* csize = labelled(desc.defineChoiceParam(kPChunkSize), "Chunk Size",
                           "Frames per model run. Auto: the longest that fits the free GPU memory. "
                           "Smaller chunks need less memory but have more seams, and the model may "
                           "pick a different main object in each chunk.");
    for (const char* c : kChunkSizeNames) csize->appendOption(c);
    csize->setDefault(0);
    csize->setEvaluateOnChange(false);
    place(*csize, gChunk);
    auto* merge = labelled(desc.defineChoiceParam(kPMerge), "Merge",
                           "How overlapping chunks are joined. Smooth: an eased dissolve. Flow Warp: "
                           "the two mattes are warped towards each other first, so edges move instead "
                           "of ghosting (a few seconds per seam).");
    for (const char* c : kMergeNames) merge->appendOption(c);
    merge->setDefault(0);
    merge->setEvaluateOnChange(false);
    place(*merge, gChunk);

    // --- ROI (4 rows; decision 63): the matte is the picture's own alpha ---
    auto* gRoi = group("grpRoi", "ROI (Alpha)", main);
    place(*label(kPRoiInfo, "ROI", "Alpha = ROI"), gRoi);
    auto* rmargin = labelled(desc.defineIntParam(kPRoiMargin), "ROI Margin (px)",
                             "Pixels of context added around the matte's bounding box over the whole "
                             "clip (at least 32). The crop is also at least 512 pixels per side and a "
                             "multiple of 32.");
    rmargin->setRange(kRoiMinMargin, 2048);
    rmargin->setDisplayRange(kRoiMinMargin, 256);
    rmargin->setDefault(64);
    place(*rmargin, gRoi);
    auto* rmask = labelled(desc.defineBooleanParam(kPRoiMask), "Black Out Outside ROI",
                           "Black out everything outside the alpha matte before inference, a stronger "
                           "cue than the crop alone. Dilate the matte upstream if it is tight.");
    rmask->setDefault(false);
    place(*rmask, gRoi);
    auto* rlimit = labelled(desc.defineBooleanParam(kPRoiLimit), "Multiply Result by ROI",
                            "Multiply the result by the ROI matte (applied when viewing, no "
                            "reprocessing needed).");
    rlimit->setDefault(false);
    place(*rlimit, gRoi);

    // --- Cache (5 rows) ---
    auto* gCache = group("grpCache", "Cache", main);
    auto* cdir = labelled(desc.defineStringParam(kPCacheDir), "Cache Folder",
                          "Where generated mattes are cached (about 4 MB per HD frame).");
    cdir->setStringType(OFX::eStringTypeDirectoryPath);
    cdir->setDefault(homeDir() + "/.cache/ltx-alphagen/mattes");
    perUser(cdir);
    place(*cdir, gCache);
    place(*label(kPCacheInfo, "Cache Size", ""), gCache);
    place(*button(kPPurgeNode, "Purge Cache", "Delete the cached mattes of this node's clip."),
          gCache);
    place(*button(kPPurgeAll, "Purge All", "Delete every cached matte of every node and project."),
          gCache);

    // --- Motion (3 rows; decisions 52, 74): the 6th column of tab 1 (Alpha Gen, Performance,
    // Chunking, ROI, Cache, Motion), so Licence/Models overflow to tab 2, which Flame names after
    // the next page ("Setup"). It took over the spacer column, and keeps its Version label, so a
    // test in Flame shows which build is loaded.
    auto* gMotion = group("grpMotion", "Motion", main);
    auto* hold = labelled(desc.defineChoiceParam(kPFrameHold), "Frame Hold",
                          "For fast motion. The model packs 8 frames into one step, so motion "
                          "within 8 frames can blur the matte. 2x/4x/8x repeat each frame so a "
                          "step holds 4/2/1 source frames; processing takes about 2/4/8 times as long. "
                          "Auto measures the motion (inside the ROI alpha, if any) and holds only "
                          "the chunks that need it.");
    for (const char* c : kFrameHoldNames) hold->appendOption(c);
    hold->setDefault(0);
    place(*hold, gMotion);
    place(*label("version", "Version", LTXAG_VERSION " (" LTXAG_GIT ")"), gMotion);

    // --- Licence (5 rows, per user) ---
    auto* gLic = group("grpLicence", "Licence", setupPage);
    place(*button(kPViewLicence, "View Licence", "Show the LTX-2.x Community License terms."),
          gLic);
    auto* accept = labelled(desc.defineBooleanParam(kPAccept), "I Accept",
                            "I accept the LTX-2.x Community License. Free below USD 10M annual "
                            "revenue; above that a paid Lightricks licence is required.");
    accept->setDefault(false);
    perUser(accept);
    place(*accept, gLic);
    place(*label(kPTokenCaption, "Token", kTokenCaptionDefault), gLic);
    auto* token = labelled(desc.defineStringParam(kPToken), "HF Token",
                           "Your Hugging Face access token (hf_...). Used for this download only, "
                           "never stored.");
    token->setDefault("");
    perUser(token);
    place(*token, gLic);
    place(*button(kPDownload, "Download",
                  "Install the LTX runtime and the models into the Model Folder: reuses verified "
                  "files from Existing Models, downloads the rest with your token."),
          gLic);

    // --- Models (5 rows, per user) ---
    auto* gModels = group("grpModels", "Models", setupPage);
    auto* dir = labelled(desc.defineStringParam(kPModelDir), "Model Folder",
                         "Where the models and the LTX runtime are installed (about 80 GB).");
    dir->setStringType(OFX::eStringTypeDirectoryPath);
    dir->setDefault(homeDir() + "/ltx-alphagen-models");
    perUser(dir);
    place(*dir, gModels);
    auto* ex = labelled(desc.defineStringParam(kPExistingDir), "Existing Models",
                        "Optional: a folder that may already contain the model files (e.g. a ComfyUI "
                        "models folder). Matching files are verified and copied instead of downloaded.");
    ex->setStringType(OFX::eStringTypeDirectoryPath);
    ex->setDefault("");
    perUser(ex);
    place(*ex, gModels);
    place(*label(kPSetupStatus, "Setup", ""), gModels);

    // --- Support (2 rows, per user) ---
    auto* gSupport = group("grpSupport", "Support", setupPage);
    auto* dbg = labelled(desc.defineBooleanParam(kPDebugLog), "Debug Logging",
                         "Verbose logging for troubleshooting (per user).");
    dbg->setDefault(false);
    perUser(dbg);
    place(*dbg, gSupport);
    place(*button(kPCollectLogs, "Collect Logs",
                  "Write a support bundle (logs and system details) to your home folder."),
          gSupport);

    // --- Prototype (dev builds; hidden otherwise) ---
    OFX::GroupParamDescriptor* gProto = proto ? group("grpProto", "Prototype", proto) : nullptr;
    auto* secs = labelled(desc.defineDoubleParam(kPSimSeconds), "Sim Seconds",
                          "Simulated processing time per chunk.");
    secs->setRange(0.0, 3600.0);
    secs->setDisplayRange(0.0, 120.0);
    secs->setDefault(20.0);
    auto* sim = labelled(desc.defineChoiceParam(kPSimulate), "Simulate",
                         "Make the next Process run fail on purpose.");
    for (const char* s : kSimulations) sim->appendOption(s);
    sim->setDefault(0);
    auto* needp = labelled(desc.defineDoubleParam(kPSimNeed), "Sim VRAM GB",
                           "Pretend the run needs this much GPU memory (0 = skip the check).");
    needp->setRange(0.0, 1024.0);
    needp->setDisplayRange(0.0, 128.0);
    needp->setDefault(0.0);
    auto* py = labelled(desc.defineStringParam(kPPython), "Worker Python",
                        "Python used for the worker and helpers.");
    py->setStringType(OFX::eStringTypeFilePath);
    py->setDefault("/usr/bin/python3");
    auto* gate = labelled(desc.defineBooleanParam(kPEnforceGate), "Enforce Gate",
                          "Apply the licence/model gate as a release build would.");
    gate->setDefault(false);
    auto* refresh = labelled(desc.defineBooleanParam(kPAutoRefresh), "Auto Refresh",
                             "EXPERIMENT: refresh the viewer when processing finishes (sets a "
                             "param from a background thread).");
    refresh->setDefault(true);
    auto* dummy = labelled(desc.defineBooleanParam(kPUseDummy), "Use Dummy",
                           "Use the dummy worker (luminance, no GPU) instead of the real model. "
                           "Sim Seconds and Simulate only apply to the dummy.");
    dummy->setDefault(false);
    for (OFX::ValueParamDescriptor* p :
         std::vector<OFX::ValueParamDescriptor*>{secs, sim, needp, py, gate, refresh, dummy}) {
      p->setEvaluateOnChange(false);
      if (gProto)
        place(*p, gProto);
      else
        p->setIsSecret(true);
    }

    // --- Hidden ---
    auto* legacy = desc.defineChoiceParam(kPColour);  // the old Input Colour (saved setups load)
    for (const char* c : kLegacyColourSpaces) legacy->appendOption(c);
    legacy->setDefault(0);
    legacy->setIsSecret(true);
    legacy->setEvaluateOnChange(false);
    auto* roist = desc.defineStringParam(kPRoiState);
    roist->setDefault("");
    roist->setIsSecret(true);
    roist->setEvaluateOnChange(false);
    auto* init = desc.defineBooleanParam(kPInitialized);
    init->setDefault(false);
    init->setIsSecret(true);
    init->setEvaluateOnChange(false);
    auto* nid = desc.defineStringParam(kPNodeId);
    nid->setDefault("");
    nid->setIsSecret(true);
    nid->setEvaluateOnChange(false);
    auto* tick = desc.defineIntParam(kPRefreshTick);
    tick->setDefault(0);
    tick->setIsSecret(true);
    tick->setIsPersistant(false);
    tick->setCanUndo(false);
    tick->setEvaluateOnChange(true);  // the point: a change makes Flame re-render
  }

  OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override {
    return new AlphaGenEffect(handle);
  }
};

}  // namespace

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
  static AlphaGenFactory factory;
  ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
