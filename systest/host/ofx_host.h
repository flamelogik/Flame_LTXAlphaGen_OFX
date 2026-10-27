// A minimal OFX host for system tests (systest/README.md). It loads the plugin bundle the way
// Flame does and imitates the Flame behaviour recorded in ofx/CLAUDE.md ("Flame host behaviour"):
// - every action runs on the calling (main) thread;
// - every plugin setValue is reported straight back as instanceChanged (reason plugin edit),
//   and a recursion deeper than kMaxEchoDepth is recorded as an error (Flame crashed there);
// - param values set from another thread are applied only at the next "UI interaction"
//   (pumpUi(), which every user action and render calls first);
// - the output clip stays RGBA whatever getClipPreferences asks for;
// - sendMessage questions get a scripted Yes/No/no answer; messages are recorded.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"

namespace systest {

struct Prop {
  enum Type { Int, Dbl, Str, Ptr } type = Int;
  std::vector<int> i;
  std::vector<double> d;
  std::vector<std::string> s;
  std::vector<void*> p;
  int size() const;
};

// One OFX property set. Sets create or overwrite; gets of unknown names fail (kOfxStatErrUnknown).
struct PropSet {
  std::map<std::string, Prop> props;
  void setInt(const char* n, int v, int idx = 0);
  void setDbl(const char* n, double v, int idx = 0);
  void setStr(const char* n, const std::string& v, int idx = 0);
  void setPtr(const char* n, void* v, int idx = 0);
  int getInt(const char* n, int idx = 0, int def = 0) const;
  double getDbl(const char* n, int idx = 0, double def = 0) const;
  std::string getStr(const char* n, int idx = 0) const;
};

class Instance;

struct Param {
  PropSet props;  // first member: the param's property set handle is &props
  std::string name, type;
  int iv = 0;
  double dv = 0.0;
  std::string sv;
  Instance* owner = nullptr;  // null for descriptor params
};

struct ParamSet {
  PropSet props;
  std::vector<std::unique_ptr<Param>> list;
  std::map<std::string, Param*> byName;
  Instance* owner = nullptr;
};

// An image handed to the plugin; the handle is &props.
struct Image {
  PropSet props;
  std::vector<float> pixels;  // RGBA float, bottom-up rows, as Flame delivers
  int w = 0, h = 0;
};

// The source frames: RGBA float, row 0 = top of the picture (converted to OFX bottom-up).
using FrameSource = std::function<void(int frame, int w, int h, std::vector<float>& rgbaTopDown)>;

struct Clip {
  PropSet props;
  std::string name;
  Instance* owner = nullptr;
};

struct Effect {
  PropSet props;
  ParamSet params;
  std::map<std::string, std::unique_ptr<Clip>> clips;
  Instance* instance = nullptr;  // null for the descriptor
};

struct Message {
  std::string type, text;
};

enum class Reply { Yes, No, None };

// A rendered output frame (RGBA, row 0 = top), plus the footer message after the render.
struct Rendered {
  OfxStatus status = kOfxStatFailed;
  int w = 0, h = 0;
  std::vector<float> rgba;
  const float* px(int x, int row) const { return &rgba[(size_t(row) * w + x) * 4]; }
};

class Host;

class Instance {
 public:
  Instance(Host& host, Effect& descriptor, int w, int h, int frames, FrameSource src);
  ~Instance();

  // A user edit: value change, then instanceChanged with reason "user edited".
  void setInt(const std::string& name, int v);
  void setBool(const std::string& name, bool v) { setInt(name, v ? 1 : 0); }
  void setDouble(const std::string& name, double v);
  void setString(const std::string& name, const std::string& v);
  void press(const std::string& name);  // a push button

  int getInt(const std::string& name);
  double getDouble(const std::string& name);
  std::string getString(const std::string& name);
  bool enabled(const std::string& name);
  bool has(const std::string& name) const { return effect_.params.byName.count(name) > 0; }

  Rendered render(int frame);
  void pumpUi();  // apply param changes made from non-action threads (Flame: at the next interaction)
  PropSet clipPreferences();  // GetClipPreferences again (Flame: after a clip-preferences slave param changes)

  std::vector<Message> messages;  // sendMessage, in order
  std::string persistent;         // the current footer message ("" = none)
  std::vector<Reply> replies;     // answers to the next questions; empty -> defaultReply
  Reply defaultReply = Reply::None;
  int echoes = 0;                 // plugin setValues reported back as instanceChanged
  int maxEchoDepth = 0;

  // Host internals (called from the suites).
  Effect& effect() { return effect_; }
  int width() const { return w_; }
  int height() const { return h_; }
  int frames() const { return frames_; }
  Image* makeSourceImage(int frame);
  Image* outputImage() { return output_.get(); }
  void onPluginSetValue(Param& p);  // from paramSetValue
  void queueBackgroundSet(Param& p, int iv, double dv, const std::string& sv);
  OfxStatus dispatch(const char* action, PropSet* in, PropSet* out);

 private:
  void changed(const std::string& name, const char* reason);
  Param& param(const std::string& name);

  Host& host_;
  Effect effect_;
  int w_, h_, frames_;
  FrameSource src_;
  std::unique_ptr<Image> output_;
  int depth_ = 0;
  std::mutex queueMutex_;
  struct Pending {
    Param* p;
    int iv;
    double dv;
    std::string sv;
  };
  std::vector<Pending> queue_;
};

class Host {
 public:
  explicit Host(const std::string& binaryPath);
  ~Host();
  bool loaded() const { return plugin_ != nullptr; }
  const std::string& loadError() const { return error_; }
  OfxPlugin* plugin() { return plugin_; }

  // load + describe + describeInContext(Filter). Returns the status of the first failure.
  OfxStatus describe();
  Effect& descriptor() { return descriptor_; }
  std::unique_ptr<Instance> createInstance(int w, int h, int frames, FrameSource src);

  std::thread::id mainThread() const { return main_; }
  std::vector<std::string> errors;  // host-detected violations (recursion, wrong thread, ...)
  std::set<std::string> unknownProps;  // property gets the host could not answer (diagnosis)
  std::map<std::string, int> savedValues;  // int/choice/bool values a new instance loads with (a saved setup)
  PropSet hostProps;
  OfxHost ofxHost{};

 private:
  void* lib_ = nullptr;
  OfxPlugin* plugin_ = nullptr;
  std::string error_;
  Effect descriptor_;
  std::thread::id main_;
  bool loadedAction_ = false;
};

constexpr int kMaxEchoDepth = 32;

}  // namespace systest
