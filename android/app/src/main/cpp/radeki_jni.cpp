#include <jni.h>

#ifdef __ANDROID__
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

#include <fstream>
#include <iterator>
#include <string>

#include "binary_analysis/analysis.h"
#include "compat/matrix.h"
#include "ipa/bundle.h"
#include "core/json.h"
#include "relinker/relinker.h"
#include "runtime/runtime.h"
#include "runtime/framework_stubs.h"
#include "runtime/stub_dispatch.h"
#include "synth_macho.h"

using namespace radeki;

static std::string run(const std::string& path) {
  JsonWriter j;
  j.beginObject();
  try {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot open extracted executable");
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(f), {}};
    auto slices = macho::listSlices(file);
    auto ch = macho::chooseSliceForHost(slices);
    if (!ch.index) throw FormatError(ch.reason);
    auto img = macho::parseSlice(file, slices[*ch.index]);
    auto rep = analysis::analyze(img);
    j.kv("arch", macho::archName(img.arch)).kvb("encrypted", img.cryptId != 0);
    j.kvb("objc", rep.objc).kvb("swift", rep.swift).kvb("metal", rep.metal).kvb("opengles", rep.gles).kvb("audio", rep.audio);
    j.kvu("imports", img.imports.size());
    j.key("blockers").beginArray();
    for (auto& b : rep.blockers) j.str(b);
    j.endArray();
    compat::Detected detected;
    detected.objc=rep.objc;detected.gles=rep.gles;detected.metal=rep.metal;
    detected.audio=rep.audio;detected.swift=rep.swift;
    for(const auto& f:rep.frameworks){
      if(f.find("UIKit")!=std::string::npos)detected.uikit=true;
      if(f.find("CoreFoundation")!=std::string::npos)detected.corefoundation=true;
      if(f.find("Vulkan")!=std::string::npos)detected.vulkan=true;
    }
    detected.arm64e=img.arch==macho::Arch::ARM64e;
    detected.armv7=img.arch==macho::Arch::ARMv7;
#if defined(__arm__) && !defined(__aarch64__)
    detected.hostArm32=true;
#endif
    detected.encrypted=img.cryptId!=0;
    if(detected.encrypted)detected.blockedReason="encrypted executable";
    size_t totalImports=img.imports.size();
    try {
      auto registry = runtime::makeCompatRegistry();
      relinker::LinkOptions o;
      o.resolver = &registry;
      auto li = relinker::link(img, o);
      detected.linked=true;
      // Analysis links without the dispatch-stub factory, so traps and unresolved strong
      // imports coincide; count the imports directly to stay honest if that ever changes.
      size_t strong=0;
      for(const auto& imp:li.imports){
        if(!imp.resolved&&!imp.weak)++strong;
        if(imp.weak&&!imp.resolved)++detected.unresolvedWeak;
      }
      detected.unresolvedStrong=strong;
      j.kvb("linked", true).kvu("unresolvedImports", strong);
    } catch (const relinker::LinkError& e) {
      j.kvb("linked", false).kv("linkError", e.what());
    }
    j.kv("state",compat::runtimeStateName(compat::stateFor(detected)))
      .kv("summary",compat::summaryFor(detected,totalImports));
    j.key("capabilities").beginArray();
    for(const auto& row:compat::neededCapabilities(detected))j.beginObject().kv("id",row.id)
      .kv("title",row.title).kv("status",compat::statusName(row.status)).kv("detail",row.detail).endObject();
    j.endArray();
  } catch (const std::exception& e) {
    j.kv("error", e.what());
  }
  j.endObject();
  return j.str();
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_beginRunLog(
    JNIEnv* env, jclass, jstring path, jboolean realtimeLogging) {
  const char* p = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
  runtime::beginRunLog(p ? p : "", realtimeLogging == JNI_TRUE);
  runtime::logRunEvent("run session initialized before Android Surface binding");
  if (p) env->ReleaseStringUTFChars(path, p);
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_endRunLog(JNIEnv*, jclass) {
  runtime::logRunEvent("native run logger closing");
  runtime::endRunLog();
}

extern "C" JNIEXPORT jstring JNICALL Java_org_radekiosnative_recompiler_Native_liveGraphicsStatus(
    JNIEnv* env, jclass) {
  const auto s = runtime::currentGraphicsStatus();
  JsonWriter j;
  j.beginObject().kvb("eventLoopActive", s.eventLoopActive).kvb("nativeWindowBound", s.nativeWindowBound)
    .kvb("eglReady", s.eglReady).kvb("windowSurface", s.windowSurface)
    .kvb("contextCurrent", s.contextCurrent).kvb("displayLinkRegistered", s.displayLinkRegistered)
    .key("width").num(s.width).key("height").num(s.height)
    .kvu("uiFrames", s.uiFrames).kvu("successfulSwaps", s.presentedFrames)
    .kv("lastGraphicsIssue", s.lastEglIssue).endObject();
  return env->NewStringUTF(j.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_org_radekiosnative_recompiler_Native_analyze(JNIEnv* env, jclass, jstring path) {
  const char* p = env->GetStringUTFChars(path, nullptr);
  std::string r = run(p);
  env->ReleaseStringUTFChars(path, p);
  return env->NewStringUTF(r.c_str());
}

static std::string resultJson(const runtime::RunResult& r) {
  JsonWriter j;
  j.beginObject().kvb("ran", r.ran).kvb("crashed", r.crashed).key("exitCode").num(r.exitCode);
  j.kv("signal", r.signalName).kv("trapSymbol", r.trapSymbol).kv("error", r.error).kv("output", r.output);
  j.kvh("faultPc", r.faultPc).kvh("faultAddr", r.faultAddr).kv("reason",runtime::describeResult(r))
    .kv("trapClass",r.trapClass).kv("subsystem",r.subsystem).kv("resolutionMethod",r.resolutionMethod)
    .kvh("callAddress",r.callAddress).kv("callSymbol",r.callSymbol).kv("image",r.image)
    .key("threadId").num(r.threadId).kvu("dispatchStubs", r.dispatchStubs);
  j.key("recentCalls").beginArray();for(const auto& c:r.recentCalls)j.str(c);j.endArray();
  const auto graphics = runtime::currentGraphicsStatus();
  j.key("graphics").beginObject().kvb("eventLoopActive", graphics.eventLoopActive)
    .kvb("nativeWindowBound", graphics.nativeWindowBound).kvb("eglReady", graphics.eglReady)
    .kvb("windowSurface", graphics.windowSurface).kvb("contextCurrent", graphics.contextCurrent)
    .kvb("displayLinkRegistered", graphics.displayLinkRegistered).key("width").num(graphics.width)
    .key("height").num(graphics.height).kvu("uiFrames", graphics.uiFrames)
    .kvu("successfulSwaps", graphics.presentedFrames).kv("lastGraphicsIssue", graphics.lastEglIssue).endObject();
  j.endObject();
  return j.str();
}

extern "C" JNIEXPORT jstring JNICALL Java_org_radekiosnative_recompiler_Native_run(
    JNIEnv* env, jclass, jstring path, jboolean compatibilityFallbacks, jboolean traceMissingApis) {
  const char* p = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
  runtime::RunResult r;
  try {
    runtime::logRunEvent(std::string("native run requested for ") + (p ? p : "<null path>"));
    runtime::ensureSandboxDirectories();
    if (!p) throw FormatError("missing executable path");
    std::ifstream f(p, std::ios::binary);
    if (!f) throw FormatError("cannot open extracted executable");
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(f), {}};
    auto slices = macho::listSlices(file);
    auto ch = macho::chooseSliceForHost(slices);
    if (!ch.index) throw FormatError(ch.reason);
    auto img = macho::parseSlice(file, slices[*ch.index]);
    runtime::logRunEvent(std::string("Mach-O selected: ") + macho::archName(img.arch) +
        ", imports=" + std::to_string(img.imports.size()) + ", segments=" + std::to_string(img.segments.size()));
    // Compatibility mode enables the host shims and unresolved-import dispatch stubs. RTLS also
    // enables their sampled first-call log entries, without changing stub return behavior.
    runtime::setStubCallLogging(traceMissingApis == JNI_TRUE || runtime::realtimeLoggingEnabled());
    runtime::logRunEvent(std::string("guest launch options: compatibility fallbacks=") +
        (compatibilityFallbacks == JNI_TRUE ? "on" : "off") +
        ", missing API trace=" + (traceMissingApis == JNI_TRUE ? "on" : "off"));
    r = runtime::runImage(img, compatibilityFallbacks == JNI_TRUE);
    r.image = p;
    runtime::logRunEvent("guest execution result: " + runtime::describeResult(r));
  } catch (const std::exception& e) {
    r.error = e.what();
    runtime::logRunEvent(std::string("native run error: ") + e.what());
  }
  if (p) env->ReleaseStringUTFChars(path, p);
  return env->NewStringUTF(resultJson(r).c_str());
}

// Runs the synthetic ARM64 test program natively on this device.
extern "C" JNIEXPORT jstring JNICALL Java_org_radekiosnative_recompiler_Native_selfTest(JNIEnv* env, jclass) {
  runtime::RunResult r;
  try {
    runtime::logRunEvent("starting built-in native self-test");
#if defined(__arm__) && !defined(__aarch64__)
    auto bytes = synth::buildArm32Test();
#else
    auto bytes = synth::build({});
#endif
    auto img = macho::parseFile(bytes);
    runtime::setStubCallLogging(runtime::realtimeLoggingEnabled());
    r = runtime::runImage(img);
    r.image = "synthetic self-test";
    runtime::logRunEvent("self-test result: " + runtime::describeResult(r));
  } catch (const std::exception& e) {
    r.error = e.what();
    runtime::logRunEvent(std::string("self-test error: ") + e.what());
  }
  return env->NewStringUTF(resultJson(r).c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_org_radekiosnative_recompiler_Native_inspectBundle(
    JNIEnv* env,jclass,jstring app,jstring icon) {
  const char* a=env->GetStringUTFChars(app,nullptr);
  const char* dest=env->GetStringUTFChars(icon,nullptr);
  ipa::BundleInfo info;std::string error;
  bool ok=ipa::inspectBundle(a,dest,info,error);
  env->ReleaseStringUTFChars(app,a);env->ReleaseStringUTFChars(icon,dest);
  JsonWriter j;j.beginObject().kvb("ok",ok);
  if(ok){
    j.kv("bundleId",info.bundleIdentifier).kv("name",info.name).kv("version",info.version)
     .kv("shortVersion",info.shortVersion).kv("executable",info.executable)
     .kv("executablePath",info.executablePath).kvu("executableSize",info.executableSize)
     .kv("iconPath",info.iconPath).kv("minimumOSVersion",info.minimumOSVersion).kv("platform",info.platform);
    j.key("architectures").beginArray();for(const auto& arch:info.architectures)j.str(arch);j.endArray();
    j.key("warnings").beginArray();for(const auto& w:info.warnings)j.str(w);j.endArray();
  }else j.kv("error",error);
  j.endObject();return env->NewStringUTF(j.str().c_str());
}

// Bind an Android Surface (cast to ANativeWindow) to the EGL bridge. Called when the
// SurfaceTexture/SurfaceView is created or changes size.
extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_bindSurface(
    JNIEnv* env, jclass, jobject surface, jint width, jint height) {
#ifdef __ANDROID__
  if (!surface) {
    radeki::runtime::updateAndroidEglWindow(nullptr, width > 0 ? width : 0, height > 0 ? height : 0);
    radeki::runtime::requestExitUiLoop(0);
    return;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) {
    runtime::logRunEvent("ANativeWindow_fromSurface failed for the launch Surface");
    runtime::updateAndroidEglWindow(nullptr, width, height);
    return;
  }
  if (width <= 0 || height <= 0) {
    width = ANativeWindow_getWidth(window);
    height = ANativeWindow_getHeight(window);
  }
  runtime::logRunEvent(std::string("Native Android Surface received: ") + std::to_string(width) + "x" + std::to_string(height));
  runtime::bindAndroidEglWindow(window, width, height);
#else
  (void)env; (void)surface; (void)width; (void)height;
#endif
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_surfaceChanged(
    JNIEnv* env, jclass, jobject surface, jint width, jint height) {
#ifdef __ANDROID__
  if (!surface) {
    radeki::runtime::updateAndroidEglWindow(nullptr, 0, 0);
    return;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) {
    runtime::logRunEvent("ANativeWindow_fromSurface failed while resizing the Surface");
    runtime::updateAndroidEglWindow(nullptr, 0, 0);
    return;
  }
  if (width <= 0 || height <= 0) {
    width = ANativeWindow_getWidth(window);
    height = ANativeWindow_getHeight(window);
  }
  runtime::updateAndroidEglWindow(window, width, height);
#else
  (void)env; (void)surface; (void)width; (void)height;
#endif
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_surfaceDestroyed(
    JNIEnv*, jclass) {
  runtime::logRunEvent("Android Surface destroyed; requesting guest event-loop exit");
  runtime::updateAndroidEglWindow(nullptr, 0, 0);
  runtime::requestExitUiLoop(0);
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_touchEvent(
    JNIEnv*, jclass, jint action, jfloat x, jfloat y, jlong timeMs) {
  radeki::runtime::injectTouchEvent(static_cast<int>(action), x, y, static_cast<int64_t>(timeMs));
}

extern "C" JNIEXPORT void JNICALL Java_org_radekiosnative_recompiler_Native_requestExit(
    JNIEnv*, jclass, jint code) {
  radeki::runtime::requestExitUiLoop(static_cast<int>(code));
}
