// The OFX suites of the system-test host (ofx_host.h). Only what the plugin's Support library
// fetches is provided: property, parameter, image effect, memory, multithread, message V1/V2.
// Signatures follow the vendored headers in third_party/openfx/include.
#include "ofx_host.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

namespace systest {

namespace {

Host* gHost = nullptr;
std::mutex gValueMutex;  // param values: the plugin's watcher threads read and write them too
std::map<PropSet*, std::unique_ptr<Image>> gImages;  // source images handed out, by handle
std::mutex gImageMutex;

PropSet* P(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
OfxPropertySetHandle H(PropSet* p) { return reinterpret_cast<OfxPropertySetHandle>(p); }

// --- property suite ---

template <class F>
OfxStatus withProp(OfxPropertySetHandle h, const char* name, int idx, F f) {
  if (!h) return kOfxStatErrBadHandle;
  auto it = P(h)->props.find(name);
  if (it == P(h)->props.end()) {
    if (gHost) gHost->unknownProps.insert(name);
    return kOfxStatErrUnknown;
  }
  if (idx < 0 || idx >= it->second.size()) return kOfxStatErrBadIndex;
  return f(it->second);
}

Prop& slot(OfxPropertySetHandle h, const char* name, Prop::Type t, int idx) {
  Prop& p = P(h)->props[name];
  if (p.type != t && p.size() == 0) p.type = t;
  p.type = t;
  switch (t) {
    case Prop::Int: if (int(p.i.size()) <= idx) p.i.resize(idx + 1); break;
    case Prop::Dbl: if (int(p.d.size()) <= idx) p.d.resize(idx + 1); break;
    case Prop::Str: if (int(p.s.size()) <= idx) p.s.resize(idx + 1); break;
    case Prop::Ptr: if (int(p.p.size()) <= idx) p.p.resize(idx + 1); break;
  }
  return p;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v) {
  if (!h) return kOfxStatErrBadHandle;
  slot(h, n, Prop::Ptr, i).p[i] = v;
  return kOfxStatOK;
}
OfxStatus propSetString(OfxPropertySetHandle h, const char* n, int i, const char* v) {
  if (!h) return kOfxStatErrBadHandle;
  slot(h, n, Prop::Str, i).s[i] = v ? v : "";
  return kOfxStatOK;
}
OfxStatus propSetDouble(OfxPropertySetHandle h, const char* n, int i, double v) {
  if (!h) return kOfxStatErrBadHandle;
  slot(h, n, Prop::Dbl, i).d[i] = v;
  return kOfxStatOK;
}
OfxStatus propSetInt(OfxPropertySetHandle h, const char* n, int i, int v) {
  if (!h) return kOfxStatErrBadHandle;
  slot(h, n, Prop::Int, i).i[i] = v;
  return kOfxStatOK;
}
OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) {
  if (!h) return kOfxStatErrBadHandle;
  P(h)->props.erase(n);
  for (int i = 0; i < c; ++i) propSetPointer(h, n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) {
  if (!h) return kOfxStatErrBadHandle;
  P(h)->props.erase(n);
  for (int i = 0; i < c; ++i) propSetString(h, n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) {
  if (!h) return kOfxStatErrBadHandle;
  P(h)->props.erase(n);
  for (int i = 0; i < c; ++i) propSetDouble(h, n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) {
  if (!h) return kOfxStatErrBadHandle;
  P(h)->props.erase(n);
  for (int i = 0; i < c; ++i) propSetInt(h, n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v) {
  return withProp(h, n, i, [&](Prop& p) {
    if (p.type != Prop::Ptr) return kOfxStatErrValue;
    *v = p.p[i];
    return kOfxStatOK;
  });
}
OfxStatus propGetString(OfxPropertySetHandle h, const char* n, int i, char** v) {
  return withProp(h, n, i, [&](Prop& p) {
    if (p.type != Prop::Str) return kOfxStatErrValue;
    *v = const_cast<char*>(p.s[i].c_str());
    return kOfxStatOK;
  });
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v) {
  return withProp(h, n, i, [&](Prop& p) {
    if (p.type == Prop::Dbl) *v = p.d[i];
    else if (p.type == Prop::Int) *v = p.i[i];
    else return kOfxStatErrValue;
    return kOfxStatOK;
  });
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) {
  return withProp(h, n, i, [&](Prop& p) {
    if (p.type == Prop::Int) *v = p.i[i];
    else if (p.type == Prop::Dbl) *v = int(p.d[i]);
    else return kOfxStatErrValue;
    return kOfxStatOK;
  });
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) {
  for (int i = 0; i < c; ++i)
    if (OfxStatus s = propGetPointer(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v) {
  for (int i = 0; i < c; ++i)
    if (OfxStatus s = propGetString(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) {
  for (int i = 0; i < c; ++i)
    if (OfxStatus s = propGetDouble(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) {
  for (int i = 0; i < c; ++i)
    if (OfxStatus s = propGetInt(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
OfxStatus propReset(OfxPropertySetHandle h, const char* n) {
  if (!h) return kOfxStatErrBadHandle;
  return P(h)->props.count(n) ? kOfxStatOK : kOfxStatErrUnknown;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char* n, int* c) {
  if (!h) return kOfxStatErrBadHandle;
  // A real host predefines every descriptor property (dimension 0 until set); this host
  // creates them on first set, so an unset one reports dimension 0 too.
  auto it = P(h)->props.find(n);
  *c = it == P(h)->props.end() ? 0 : it->second.size();
  return kOfxStatOK;
}

OfxPropertySuiteV1 gPropSuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN,
    propSetDoubleN, propSetIntN, propGetPointer, propGetString, propGetDouble, propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN, propReset, propGetDimension};

// --- parameter suite ---

ParamSet* PS(OfxParamSetHandle h) { return reinterpret_cast<ParamSet*>(h); }
Param* PA(OfxParamHandle h) { return reinterpret_cast<Param*>(h); }

bool isIntLike(const std::string& t) {
  return t == kOfxParamTypeInteger || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice;
}

OfxStatus paramDefine(OfxParamSetHandle set, const char* type, const char* name, OfxPropertySetHandle* props) {
  if (!set) return kOfxStatErrBadHandle;
  if (PS(set)->byName.count(name)) return kOfxStatErrExists;
  auto p = std::make_unique<Param>();
  p->name = name;
  p->type = type;
  p->props.setStr(kOfxPropName, name);
  p->props.setStr(kOfxParamPropType, type);
  p->props.setInt(kOfxParamPropEnabled, 1);
  p->props.setInt(kOfxParamPropSecret, 0);
  if (props) *props = H(&p->props);
  PS(set)->byName[name] = p.get();
  PS(set)->list.push_back(std::move(p));
  return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle set, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
  if (!set) return kOfxStatErrBadHandle;
  auto it = PS(set)->byName.find(name);
  if (it == PS(set)->byName.end()) return kOfxStatErrUnknown;
  if (param) *param = reinterpret_cast<OfxParamHandle>(it->second);
  if (props) *props = H(&it->second->props);
  return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle set, OfxPropertySetHandle* props) {
  if (!set) return kOfxStatErrBadHandle;
  *props = H(&PS(set)->props);
  return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle param, OfxPropertySetHandle* props) {
  if (!param) return kOfxStatErrBadHandle;
  *props = H(&PA(param)->props);
  return kOfxStatOK;
}

OfxStatus getValue(Param* p, va_list ap) {
  std::lock_guard<std::mutex> lock(gValueMutex);
  if (isIntLike(p->type)) {
    *va_arg(ap, int*) = p->iv;
  } else if (p->type == kOfxParamTypeDouble) {
    *va_arg(ap, double*) = p->dv;
  } else if (p->type == kOfxParamTypeString) {
    *va_arg(ap, const char**) = p->sv.c_str();
  } else {
    return kOfxStatErrUnsupported;
  }
  return kOfxStatOK;
}
OfxStatus paramGetValue(OfxParamHandle h, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, h);
  const OfxStatus s = getValue(PA(h), ap);
  va_end(ap);
  return s;
}
OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, t);
  const OfxStatus s = getValue(PA(h), ap);
  va_end(ap);
  return s;
}
OfxStatus paramGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus paramGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }

OfxStatus setValue(Param* p, va_list ap) {
  int iv = 0;
  double dv = 0.0;
  std::string sv;
  if (isIntLike(p->type)) iv = va_arg(ap, int);
  else if (p->type == kOfxParamTypeDouble) dv = va_arg(ap, double);
  else if (p->type == kOfxParamTypeString) { const char* s = va_arg(ap, const char*); sv = s ? s : ""; }
  else return kOfxStatErrUnsupported;
  if (p->owner && std::this_thread::get_id() != gHost->mainThread()) {
    p->owner->queueBackgroundSet(*p, iv, dv, sv);  // Flame: applied at the next interaction
    return kOfxStatOK;
  }
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    p->iv = iv;
    p->dv = dv;
    p->sv = sv;
  }
  if (p->owner) p->owner->onPluginSetValue(*p);
  return kOfxStatOK;
}
OfxStatus paramSetValue(OfxParamHandle h, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, h);
  const OfxStatus s = setValue(PA(h), ap);
  va_end(ap);
  return s;
}
OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, t);
  const OfxStatus s = setValue(PA(h), ap);
  va_end(ap);
  return s;
}
OfxStatus paramGetNumKeys(OfxParamHandle, unsigned int* n) {
  *n = 0;
  return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatErrBadIndex; }
OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

OfxParameterSuiteV1 gParamSuite = {
    paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
    paramGetValueAtTime, paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime,
    paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys,
    paramCopy, paramEditBegin, paramEditEnd};

// --- image effect suite ---

Effect* E(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
Clip* C(OfxImageClipHandle h) { return reinterpret_cast<Clip*>(h); }

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* props) {
  if (!e) return kOfxStatErrBadHandle;
  *props = H(&E(e)->props);
  return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* set) {
  if (!e) return kOfxStatErrBadHandle;
  *set = reinterpret_cast<OfxParamSetHandle>(&E(e)->params);
  return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* props) {
  if (!e) return kOfxStatErrBadHandle;
  auto& c = E(e)->clips[name];
  if (!c) {
    c = std::make_unique<Clip>();
    c->name = name;
    c->props.setStr(kOfxPropName, name);
  }
  *props = H(&c->props);
  return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props) {
  if (!e) return kOfxStatErrBadHandle;
  auto it = E(e)->clips.find(name);
  if (it == E(e)->clips.end()) return kOfxStatErrUnknown;
  if (clip) *clip = reinterpret_cast<OfxImageClipHandle>(it->second.get());
  if (props) *props = H(&it->second->props);
  return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle clip, OfxPropertySetHandle* props) {
  if (!clip) return kOfxStatErrBadHandle;
  *props = H(&C(clip)->props);
  return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime time, const OfxRectD*, OfxPropertySetHandle* image) {
  if (!clip || !C(clip)->owner) return kOfxStatErrBadHandle;
  Instance* inst = C(clip)->owner;
  if (C(clip)->name == kOfxImageEffectOutputClipName) {
    if (!inst->outputImage()) return kOfxStatFailed;  // only inside render
    *image = H(&inst->outputImage()->props);
    return kOfxStatOK;
  }
  const int f = int(std::lround(time));
  if (f < 0 || f >= inst->frames()) return kOfxStatFailed;
  Image* img = inst->makeSourceImage(f);
  *image = H(&img->props);
  return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle image) {
  std::lock_guard<std::mutex> lock(gImageMutex);
  gImages.erase(P(image));  // the output image belongs to the instance: not in the map
  return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle clip, OfxTime, OfxRectD* b) {
  if (!clip || !C(clip)->owner) return kOfxStatErrBadHandle;
  *b = {0.0, 0.0, double(C(clip)->owner->width()), double(C(clip)->owner->height())};
  return kOfxStatOK;
}
int abortFn(OfxImageEffectHandle) { return 0; }
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) {
  *h = reinterpret_cast<OfxImageMemoryHandle>(std::malloc(n ? n : 1));
  return *h ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle h) {
  std::free(h);
  return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void** p) {
  *p = h;
  return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

OfxImageEffectSuiteV1 gEffectSuite = {
    getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage,
    clipReleaseImage, clipGetRegionOfDefinition, abortFn, imageMemoryAlloc, imageMemoryFree,
    imageMemoryLock, imageMemoryUnlock};

// --- memory, multithread ---

OfxStatus memoryAlloc(void*, size_t n, void** p) {
  *p = std::malloc(n ? n : 1);
  return *p ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void* p) {
  std::free(p);
  return kOfxStatOK;
}
OfxMemorySuiteV1 gMemorySuite = {memoryAlloc, memoryFree};

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
  for (unsigned i = 0; i < n; ++i) func(i, n, arg);  // serially, on the calling thread
  return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int* n) {
  *n = 1;
  return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int* i) {
  *i = 0;
  return kOfxStatOK;
}
int multiThreadIsSpawnedThread() { return 0; }
OfxStatus mutexCreate(OfxMutexHandle* m, int lockCount) {
  auto* mx = new std::recursive_mutex;
  for (int i = 0; i < lockCount; ++i) mx->lock();
  *m = reinterpret_cast<OfxMutexHandle>(mx);
  return kOfxStatOK;
}
std::recursive_mutex* MX(OfxMutexHandle m) { return reinterpret_cast<std::recursive_mutex*>(m); }
OfxStatus mutexDestroy(const OfxMutexHandle m) {
  delete MX(m);
  return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle m) {
  MX(m)->lock();
  return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle m) {
  MX(m)->unlock();
  return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle m) { return MX(m)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
OfxMultiThreadSuiteV1 gThreadSuite = {multiThread, multiThreadNumCPUs, multiThreadIndex,
                                      multiThreadIsSpawnedThread, mutexCreate, mutexDestroy,
                                      mutexLock, mutexUnLock, mutexTryLock};

// --- message suite ---

Instance* instanceOf(void* handle) {
  // The Support library passes the effect handle; a descriptor has no instance.
  return handle ? reinterpret_cast<Effect*>(handle)->instance : nullptr;
}

std::string vformat(const char* fmt, va_list ap) {
  char buf[8192];
  std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
  return buf;
}

OfxStatus messageImpl(void* handle, const char* type, const char* fmt, va_list ap) {
  const std::string text = vformat(fmt, ap);
  Instance* inst = instanceOf(handle);
  if (!inst) {
    gHost->errors.push_back(std::string("message without an instance: ") + text);
    return kOfxStatOK;
  }
  if (std::this_thread::get_id() != gHost->mainThread())
    gHost->errors.push_back("sendMessage from a non-action thread: " + text);
  inst->messages.push_back({type ? type : "", text});
  if (type && std::strcmp(type, kOfxMessageQuestion) == 0) {
    Reply r = inst->defaultReply;
    if (!inst->replies.empty()) {
      r = inst->replies.front();
      inst->replies.erase(inst->replies.begin());
    }
    if (r == Reply::Yes) return kOfxStatReplyYes;
    if (r == Reply::No) return kOfxStatReplyNo;
  }
  return kOfxStatOK;
}
OfxStatus message(void* handle, const char* type, const char*, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const OfxStatus s = messageImpl(handle, type, fmt, ap);
  va_end(ap);
  return s;
}
OfxStatus setPersistentMessage(void* handle, const char*, const char*, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::string text = vformat(fmt, ap);
  va_end(ap);
  if (Instance* inst = instanceOf(handle)) inst->persistent = text;
  return kOfxStatOK;
}
OfxStatus clearPersistentMessage(void* handle) {
  if (Instance* inst = instanceOf(handle)) inst->persistent.clear();
  return kOfxStatOK;
}
OfxMessageSuiteV1 gMessageSuite = {message};
OfxMessageSuiteV2 gMessageSuiteV2 = {message, setPersistentMessage, clearPersistentMessage};

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
  const std::string n = name;
  if (n == kOfxPropertySuite && version == 1) return &gPropSuite;
  if (n == kOfxParameterSuite && version == 1) return &gParamSuite;
  if (n == kOfxImageEffectSuite && version == 1) return &gEffectSuite;
  if (n == kOfxMemorySuite && version == 1) return &gMemorySuite;
  if (n == kOfxMultiThreadSuite && version == 1) return &gThreadSuite;
  if (n == kOfxMessageSuite && version == 1) return &gMessageSuite;
  if (n == kOfxMessageSuite && version == 2) return &gMessageSuiteV2;
  return nullptr;  // progress, timeline, parametric, interact, OpenGL: not offered (optional)
}

void setStrN(PropSet& p, const char* name, std::initializer_list<const char*> v) {
  int i = 0;
  for (const char* s : v) p.setStr(name, s, i++);
}

}  // namespace

// --- PropSet helpers ---

int Prop::size() const {
  switch (type) {
    case Int: return int(i.size());
    case Dbl: return int(d.size());
    case Str: return int(s.size());
    case Ptr: return int(p.size());
  }
  return 0;
}
void PropSet::setInt(const char* n, int v, int idx) { propSetInt(H(this), n, idx, v); }
void PropSet::setDbl(const char* n, double v, int idx) { propSetDouble(H(this), n, idx, v); }
void PropSet::setStr(const char* n, const std::string& v, int idx) { propSetString(H(this), n, idx, v.c_str()); }
void PropSet::setPtr(const char* n, void* v, int idx) { propSetPointer(H(this), n, idx, v); }
int PropSet::getInt(const char* n, int idx, int def) const {
  int v = def;
  return propGetInt(H(const_cast<PropSet*>(this)), n, idx, &v) == kOfxStatOK ? v : def;
}
double PropSet::getDbl(const char* n, int idx, double def) const {
  double v = def;
  return propGetDouble(H(const_cast<PropSet*>(this)), n, idx, &v) == kOfxStatOK ? v : def;
}
std::string PropSet::getStr(const char* n, int idx) const {
  char* v = nullptr;
  return propGetString(H(const_cast<PropSet*>(this)), n, idx, &v) == kOfxStatOK && v ? v : "";
}

// --- Host ---

Host::Host(const std::string& binaryPath) : main_(std::this_thread::get_id()) {
  gHost = this;
  PropSet& h = hostProps;
  h.setStr(kOfxPropName, "com.janklier.ltxag.systest");
  h.setStr(kOfxPropLabel, "LTXAG system test host");
  h.setInt(kOfxPropAPIVersion, 1, 0);
  h.setInt(kOfxPropAPIVersion, 4, 1);
  h.setInt(kOfxPropVersion, 1, 0);
  h.setInt(kOfxPropVersion, 0, 1);
  h.setInt(kOfxPropVersion, 0, 2);
  h.setStr(kOfxPropVersionLabel, "1.0");
  h.setInt(kOfxImageEffectHostPropIsBackground, 0);
  h.setInt(kOfxImageEffectPropSupportsOverlays, 0);
  h.setInt(kOfxImageEffectPropSupportsMultiResolution, 0);
  h.setInt(kOfxImageEffectPropSupportsTiles, 0);
  h.setInt(kOfxImageEffectPropTemporalClipAccess, 1);
  setStrN(h, kOfxImageEffectPropSupportedComponents,
          {kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha});
  setStrN(h, kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
  setStrN(h, kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthFloat});
  h.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
  h.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
  h.setInt(kOfxImageEffectPropSetableFrameRate, 0);
  h.setInt(kOfxImageEffectPropSetableFielding, 0);
  h.setInt(kOfxImageEffectInstancePropSequentialRender, 1);
  h.setInt(kOfxParamHostPropSupportsCustomInteract, 0);
  h.setInt(kOfxParamHostPropSupportsStringAnimation, 0);
  h.setInt(kOfxParamHostPropSupportsChoiceAnimation, 0);
  h.setInt(kOfxParamHostPropSupportsBooleanAnimation, 0);
  h.setInt(kOfxParamHostPropSupportsCustomAnimation, 0);
  h.setInt(kOfxParamHostPropMaxParameters, -1);
  h.setInt(kOfxParamHostPropMaxPages, 0);
  h.setInt(kOfxParamHostPropPageRowColumnCount, 5, 0);  // Flame: 5 rows, 6 columns per tab
  h.setInt(kOfxParamHostPropPageRowColumnCount, 6, 1);
  h.setStr(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
  ofxHost.host = H(&hostProps);
  ofxHost.fetchSuite = fetchSuite;

  lib_ = dlopen(binaryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib_) {
    error_ = std::string("dlopen: ") + dlerror();
    return;
  }
  auto count = reinterpret_cast<int (*)()>(dlsym(lib_, "OfxGetNumberOfPlugins"));
  auto get = reinterpret_cast<OfxPlugin* (*)(int)>(dlsym(lib_, "OfxGetPlugin"));
  if (!count || !get || count() < 1) {
    error_ = "OFX entry points missing or no plugin";
    return;
  }
  plugin_ = get(0);
  plugin_->setHost(&ofxHost);
}

Host::~Host() {
  if (plugin_ && loadedAction_) plugin_->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
  // No dlclose: the plugin's static objects may still be referenced by detached threads.
  gHost = nullptr;
}

OfxStatus Host::describe() {
  OfxStatus s = plugin_->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
  if (s != kOfxStatOK && s != kOfxStatReplyDefault) return s;
  loadedAction_ = true;
  s = plugin_->mainEntry(kOfxActionDescribe, &descriptor_, nullptr, nullptr);
  if (s != kOfxStatOK) return s;
  PropSet in;
  in.setStr(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  s = plugin_->mainEntry(kOfxImageEffectActionDescribeInContext, &descriptor_, H(&in), nullptr);
  return s;
}

std::unique_ptr<Instance> Host::createInstance(int w, int h, int frames, FrameSource src) {
  return std::make_unique<Instance>(*this, descriptor_, w, h, frames, std::move(src));
}

// --- Instance ---

Instance::Instance(Host& host, Effect& d, int w, int h, int frames, FrameSource src)
    : host_(host), w_(w), h_(h), frames_(frames), src_(std::move(src)) {
  effect_.instance = this;
  PropSet& e = effect_.props;
  e.setStr(kOfxPropType, kOfxTypeImageEffectInstance);
  e.setStr(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  e.setPtr(kOfxPropInstanceData, nullptr);
  e.setDbl(kOfxImageEffectPropProjectSize, w, 0);
  e.setDbl(kOfxImageEffectPropProjectSize, h, 1);
  e.setDbl(kOfxImageEffectPropProjectOffset, 0, 0);
  e.setDbl(kOfxImageEffectPropProjectOffset, 0, 1);
  e.setDbl(kOfxImageEffectPropProjectExtent, w, 0);
  e.setDbl(kOfxImageEffectPropProjectExtent, h, 1);
  e.setDbl(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
  e.setDbl(kOfxImageEffectInstancePropEffectDuration, frames);
  e.setInt(kOfxImageEffectInstancePropSequentialRender, 1);
  e.setInt(kOfxImageEffectPropSupportsTiles, 0);
  e.setDbl(kOfxImageEffectPropFrameRate, 24.0);
  e.setInt(kOfxPropIsInteractive, 1);
  // Params from the descriptor, at their defaults.
  for (const auto& dp : d.params.list) {
    auto p = std::make_unique<Param>();
    p->props = dp->props;
    p->name = dp->name;
    p->type = dp->type;
    p->owner = this;
    if (isIntLike(p->type)) p->iv = dp->props.getInt(kOfxParamPropDefault);
    else if (p->type == kOfxParamTypeDouble) p->dv = dp->props.getDbl(kOfxParamPropDefault);
    else if (p->type == kOfxParamTypeString) p->sv = dp->props.getStr(kOfxParamPropDefault);
    auto saved = host.savedValues.find(p->name);  // a node loaded from a saved setup
    if (saved != host.savedValues.end() && isIntLike(p->type)) p->iv = saved->second;
    effect_.params.byName[p->name] = p.get();
    effect_.params.list.push_back(std::move(p));
  }
  // Clips: Flame delivers 32-bit float RGBA; the output stays RGBA (decision 48).
  for (const auto& dc : d.clips) {
    auto c = std::make_unique<Clip>();
    c->props = dc.second->props;
    c->name = dc.first;
    c->owner = this;
    PropSet& p = c->props;
    p.setStr(kOfxPropType, kOfxTypeClip);
    p.setStr(kOfxPropLabel, dc.first);
    p.setStr(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
    p.setStr(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    p.setStr(kOfxImageClipPropUnmappedPixelDepth, kOfxBitDepthFloat);
    p.setStr(kOfxImageClipPropUnmappedComponents, kOfxImageComponentRGBA);
    p.setStr(kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
    p.setDbl(kOfxImagePropPixelAspectRatio, 1.0);
    p.setDbl(kOfxImageEffectPropFrameRate, 24.0);
    p.setDbl(kOfxImageEffectPropFrameRange, 0, 0);
    p.setDbl(kOfxImageEffectPropFrameRange, frames - 1, 1);
    p.setDbl(kOfxImageEffectPropUnmappedFrameRate, 24.0);
    p.setDbl(kOfxImageEffectPropUnmappedFrameRange, 0, 0);
    p.setDbl(kOfxImageEffectPropUnmappedFrameRange, frames - 1, 1);
    p.setStr(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
    p.setInt(kOfxImageClipPropConnected, 1);
    p.setInt(kOfxImageClipPropContinuousSamples, 0);
    effect_.clips[dc.first] = std::move(c);
  }
  const OfxStatus s = dispatch(kOfxActionCreateInstance, nullptr, nullptr);
  if (s != kOfxStatOK && s != kOfxStatReplyDefault)
    host_.errors.push_back("createInstance returned " + std::to_string(s));
  PropSet out;
  dispatch(kOfxImageEffectActionGetClipPreferences, nullptr, &out);  // the answer is ignored, as in Flame
}

PropSet Instance::clipPreferences() {
  PropSet out;
  dispatch(kOfxImageEffectActionGetClipPreferences, nullptr, &out);
  return out;
}

Instance::~Instance() {
  dispatch(kOfxActionDestroyInstance, nullptr, nullptr);
  std::lock_guard<std::mutex> lock(gImageMutex);
  gImages.clear();
}

OfxStatus Instance::dispatch(const char* action, PropSet* in, PropSet* out) {
  if (std::this_thread::get_id() != host_.mainThread())
    host_.errors.push_back(std::string("action off the main thread: ") + action);
  return host_.plugin()->mainEntry(action, &effect_, in ? H(in) : nullptr, out ? H(out) : nullptr);
}

Param& Instance::param(const std::string& name) {
  auto it = effect_.params.byName.find(name);
  if (it == effect_.params.byName.end()) throw std::runtime_error("no param " + name);
  return *it->second;
}

void Instance::changed(const std::string& name, const char* reason) {
  if (depth_ >= kMaxEchoDepth) {
    host_.errors.push_back("instanceChanged recursion deeper than " + std::to_string(kMaxEchoDepth) +
                           " at " + name + " (Flame crashed this way, CLAUDE.md)");
    return;
  }
  ++depth_;
  maxEchoDepth = std::max(maxEchoDepth, depth_);
  PropSet in;
  in.setStr(kOfxPropType, kOfxTypeParameter);
  in.setStr(kOfxPropName, name);
  in.setStr(kOfxPropChangeReason, reason);
  in.setDbl(kOfxPropTime, 0.0);
  in.setDbl(kOfxImageEffectPropRenderScale, 1.0, 0);
  in.setDbl(kOfxImageEffectPropRenderScale, 1.0, 1);
  PropSet begin;
  begin.setStr(kOfxPropChangeReason, reason);
  dispatch(kOfxActionBeginInstanceChanged, &begin, nullptr);
  const OfxStatus s = dispatch(kOfxActionInstanceChanged, &in, nullptr);
  dispatch(kOfxActionEndInstanceChanged, &begin, nullptr);
  if (s != kOfxStatOK && s != kOfxStatReplyDefault)
    host_.errors.push_back("instanceChanged(" + name + ") returned " + std::to_string(s));
  --depth_;
}

void Instance::onPluginSetValue(Param& p) {
  ++echoes;
  changed(p.name, kOfxChangePluginEdited);
}

void Instance::queueBackgroundSet(Param& p, int iv, double dv, const std::string& sv) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  queue_.push_back({&p, iv, dv, sv});
}

void Instance::pumpUi() {
  std::vector<Pending> q;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    q.swap(queue_);
  }
  for (auto& e : q) {
    {
      std::lock_guard<std::mutex> lock(gValueMutex);
      e.p->iv = e.iv;
      e.p->dv = e.dv;
      e.p->sv = e.sv;
    }
    onPluginSetValue(*e.p);
  }
}

void Instance::setInt(const std::string& name, int v) {
  pumpUi();
  Param& p = param(name);
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    p.iv = v;
  }
  changed(name, kOfxChangeUserEdited);
}
void Instance::setDouble(const std::string& name, double v) {
  pumpUi();
  Param& p = param(name);
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    p.dv = v;
  }
  changed(name, kOfxChangeUserEdited);
}
void Instance::setString(const std::string& name, const std::string& v) {
  pumpUi();
  Param& p = param(name);
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    p.sv = v;
  }
  changed(name, kOfxChangeUserEdited);
}
void Instance::press(const std::string& name) {
  pumpUi();
  param(name);
  changed(name, kOfxChangeUserEdited);
}
int Instance::getInt(const std::string& name) {
  std::lock_guard<std::mutex> lock(gValueMutex);
  return param(name).iv;
}
double Instance::getDouble(const std::string& name) {
  std::lock_guard<std::mutex> lock(gValueMutex);
  return param(name).dv;
}
std::string Instance::getString(const std::string& name) {
  std::lock_guard<std::mutex> lock(gValueMutex);
  return param(name).sv;
}
bool Instance::enabled(const std::string& name) { return param(name).props.getInt(kOfxParamPropEnabled, 0, 1) != 0; }

Image* Instance::makeSourceImage(int frame) {
  auto img = std::make_unique<Image>();
  img->w = w_;
  img->h = h_;
  std::vector<float> top(size_t(w_) * h_ * 4);
  src_(frame, w_, h_, top);
  img->pixels.resize(top.size());
  for (int r = 0; r < h_; ++r)  // OFX rows are bottom-up
    std::memcpy(&img->pixels[size_t(h_ - 1 - r) * w_ * 4], &top[size_t(r) * w_ * 4], size_t(w_) * 16);
  PropSet& p = img->props;
  p.setPtr(kOfxImagePropData, img->pixels.data());
  for (int i = 0; i < 4; ++i) {
    const int v[4] = {0, 0, w_, h_};
    p.setInt(kOfxImagePropBounds, v[i], i);
    p.setInt(kOfxImagePropRegionOfDefinition, v[i], i);
  }
  p.setInt(kOfxImagePropRowBytes, w_ * 16);
  p.setStr(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
  p.setStr(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
  p.setStr(kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
  p.setStr(kOfxImagePropField, kOfxImageFieldNone);
  p.setStr(kOfxImagePropUniqueIdentifier, "src" + std::to_string(frame));
  p.setDbl(kOfxImageEffectPropRenderScale, 1.0, 0);
  p.setDbl(kOfxImageEffectPropRenderScale, 1.0, 1);
  p.setDbl(kOfxImagePropPixelAspectRatio, 1.0);
  Image* raw = img.get();
  std::lock_guard<std::mutex> lock(gImageMutex);
  gImages[&raw->props] = std::move(img);
  return raw;
}

Rendered Instance::render(int frame) {
  pumpUi();
  output_ = std::make_unique<Image>();
  Image& o = *output_;
  o.w = w_;
  o.h = h_;
  o.pixels.assign(size_t(w_) * h_ * 4, -1.0f);  // -1: pixels the plugin did not write
  PropSet& p = o.props;
  p.setPtr(kOfxImagePropData, o.pixels.data());
  for (int i = 0; i < 4; ++i) {
    const int v[4] = {0, 0, w_, h_};
    p.setInt(kOfxImagePropBounds, v[i], i);
    p.setInt(kOfxImagePropRegionOfDefinition, v[i], i);
  }
  p.setInt(kOfxImagePropRowBytes, w_ * 16);
  p.setStr(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
  p.setStr(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
  p.setStr(kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
  p.setStr(kOfxImagePropField, kOfxImageFieldNone);
  p.setStr(kOfxImagePropUniqueIdentifier, "out");
  p.setDbl(kOfxImageEffectPropRenderScale, 1.0, 0);
  p.setDbl(kOfxImageEffectPropRenderScale, 1.0, 1);
  p.setDbl(kOfxImagePropPixelAspectRatio, 1.0);

  PropSet seq;
  seq.setDbl(kOfxImageEffectPropFrameRange, frame, 0);
  seq.setDbl(kOfxImageEffectPropFrameRange, frame, 1);
  seq.setDbl(kOfxImageEffectPropFrameStep, 1.0);
  seq.setDbl(kOfxImageEffectPropRenderScale, 1.0, 0);
  seq.setDbl(kOfxImageEffectPropRenderScale, 1.0, 1);
  seq.setInt(kOfxPropIsInteractive, 1);
  seq.setInt(kOfxImageEffectPropSequentialRenderStatus, 1);
  seq.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0);  // Flame: always 0 (CLAUDE.md)
  dispatch(kOfxImageEffectActionBeginSequenceRender, &seq, nullptr);
  PropSet in;
  in.setDbl(kOfxPropTime, frame);
  in.setDbl(kOfxImageEffectPropRenderScale, 1.0, 0);
  in.setDbl(kOfxImageEffectPropRenderScale, 1.0, 1);
  for (int i = 0; i < 4; ++i) {
    const int v[4] = {0, 0, w_, h_};
    in.setInt(kOfxImageEffectPropRenderWindow, v[i], i);
  }
  in.setStr(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
  in.setInt(kOfxImageEffectPropSequentialRenderStatus, 1);
  in.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0);
  Rendered r;
  r.status = dispatch(kOfxImageEffectActionRender, &in, nullptr);
  dispatch(kOfxImageEffectActionEndSequenceRender, &seq, nullptr);
  r.w = w_;
  r.h = h_;
  r.rgba.resize(o.pixels.size());
  for (int row = 0; row < h_; ++row)
    std::memcpy(&r.rgba[size_t(row) * w_ * 4], &o.pixels[size_t(h_ - 1 - row) * w_ * 4], size_t(w_) * 16);
  output_.reset();
  return r;
}

}  // namespace systest
