// Hand-written framework stubs + EAGL/UIKit/OpenGL ES 2.0 bridge.
#include "runtime/framework_stubs.h"
#include "runtime/runtime.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#endif
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radeki::runtime {
namespace {

size_t g_count = 0;
size_t g_glesCount = 0;

// Specialized dummy wrappers for EAGL + UIWindow + CADisplayLink + UIApplication.
DummyEAGLContext g_dummyEAGLContext;
DummyEAGLSharegroup g_dummyEAGLSharegroup;
DummyUIApplication g_dummyUIApplication;
DummyUIWindow g_dummyUIWindow;
DummyCADisplayLink g_dummyCADisplayLink;
DummyEAGLContext* g_currentEAGLContext = nullptr;
DummyUITouch g_dummyUITouch;
DummyUIEvent g_dummyUIEvent;
DummyUIScreen g_dummyUIScreen;

std::mutex g_bridgeMutex;
void* g_androidNativeWindow = nullptr;
int g_surfaceWidth = 1280;
int g_surfaceHeight = 720;
std::atomic<bool> g_uiLoopShouldExit{false};
std::atomic<bool> g_uiLoopActive{false};
std::atomic<int> g_uiLoopExitCode{0};
std::atomic<uint64_t> g_frameCounter{0};
std::atomic<int> g_lastGraphicsError{0};
std::atomic<const char*> g_lastGraphicsStage{nullptr};
bool g_eglWindowSurface = false;

// --- EGL thread ownership (EGL_BAD_ACCESS 0x3002 prevention) --------------------------------
// eglMakeCurrent/eglSwapBuffers are only legal on the thread that owns the binding. Android
// Surface callbacks arrive on the UI thread while the guest render thread holds the context
// current; touching the binding from the UI thread there raises EGL_BAD_ACCESS (0x3002).
// We therefore record the designated render thread and queue surface rebinds for it: the
// UIApplicationMain frame loop drains the queue, so every eglMakeCurrent we issue happens on
// the render thread (or inline from a thread when nothing else owns the binding).
int64_t g_eglOwnerTid = 0;                    // guarded by g_bridgeMutex; 0 = no owner
std::atomic<bool> g_eglRebindPending{false};  // a rebind is queued for the render thread
std::atomic<uint64_t> g_engineForcedSwaps{0}; // frames the loop presented after the render step
std::atomic<uint32_t> g_heartbeatFps{60};     // current engine frame heartbeat target

int64_t hostThreadId() {
#ifdef SYS_gettid
  return static_cast<int64_t>(::syscall(SYS_gettid));
#else
  return static_cast<int64_t>(::getpid());
#endif
}

// Must hold g_bridgeMutex. True when the EGL binding is owned by another thread: every
// unbind/rebind must then be deferred to that owner, never executed here.
bool eglBindingOwnedByOtherThreadLocked() {
  return g_eglOwnerTid != 0 && g_eglOwnerTid != hostThreadId();
}

// --- Touch event queue: Android MotionEvent -> iOS UITouch forwarding ----------------------
// iOS UITouch carries phase (0=began, 1=moved, 2=ended/cancelled), a view-relative location in
// points, a timestamp (NSTimeInterval seconds since system boot), and a pointer identifier. We
// keep a bounded ring of the most recent events so the game loop can drain them each frame.
enum { kTouchBegan = 0, kTouchMoved = 1, kTouchEnded = 2, kTouchCancelled = 3 };
struct TouchEvent {
  int action;       // kTouchBegan/Moved/Ended/Cancelled
  float x, y;       // pixels
  int64_t timeMs;
  int pointerId;
};
struct TouchQueue {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<TouchEvent> pending;
  static constexpr size_t kMax = 256;
  void push(TouchEvent e) {
    std::lock_guard<std::mutex> l(mu);
    if (pending.size() >= kMax) pending.pop_front();
    pending.push_back(e);
    cv.notify_one();
  }
  std::vector<TouchEvent> drain() {
    std::lock_guard<std::mutex> l(mu);
    std::vector<TouchEvent> out(pending.begin(), pending.end());
    pending.clear();
    return out;
  }
};
TouchQueue& touchQueue() { static TouchQueue q; return q; }

// Last-known active touch location (single-touch primary pointer, which is what MCPE uses).
std::atomic<int> g_activePointerId{-1};
std::atomic<float> g_lastTouchX{0}, g_lastTouchY{0};
std::atomic<int> g_lastTouchAction{-1};

// Dynamic handles to Android's native libEGL.so and libGLESv2.so (if present on host/device).
struct NativeGles {
  void* eglLib = nullptr;
  void* glesLib = nullptr;
  // Minimal EGL function pointers resolved dynamically so this file compiles on both Android
  // NDK and headless Linux hosts without requiring EGL headers at compile time.
  using EglGetDisplayFn = void* (*)(void*);
  using EglInitFn = int (*)(void*, int*, int*);
  using EglChooseConfigFn = int (*)(void*, const int*, void**, int, int*);
  using EglCreatePbufferSurfaceFn = void* (*)(void*, void*, const int*);
  using EglCreateWindowSurfaceFn = void* (*)(void*, void*, void*, const int*);
  using EglBindApiFn = int (*)(unsigned int);
  using EglCreateContextFn = void* (*)(void*, void*, void*, const int*);
  using EglMakeCurrentFn = int (*)(void*, void*, void*, void*);
  using EglSwapBuffersFn = int (*)(void*, void*);
  using EglGetCurrentContextFn = void* (*)();
  using EglGetConfigAttribFn = int (*)(void*, void*, int, int*);
  using EglGetErrorFn = int (*)();

  EglGetDisplayFn eglGetDisplay = nullptr;
  EglInitFn eglInitialize = nullptr;
  EglChooseConfigFn eglChooseConfig = nullptr;
  EglCreatePbufferSurfaceFn eglCreatePbufferSurface = nullptr;
  EglCreateWindowSurfaceFn eglCreateWindowSurface = nullptr;
  EglBindApiFn eglBindAPI = nullptr;
  EglCreateContextFn eglCreateContext = nullptr;
  EglMakeCurrentFn eglMakeCurrent = nullptr;
  EglSwapBuffersFn eglSwapBuffers = nullptr;
  EglGetCurrentContextFn eglGetCurrentContext = nullptr;
  EglGetConfigAttribFn eglGetConfigAttrib = nullptr;
  EglGetErrorFn eglGetError = nullptr;

  NativeGles() {
    for (const char* name : {"libEGL.so", "libEGL.so.1"}) {
      eglLib = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
      if (eglLib) break;
    }
    for (const char* name : {"libGLESv2.so", "libGLESv2.so.2"}) {
      glesLib = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
      if (glesLib) break;
    }
    auto symEgl = [&](const char* s) -> void* {
      if (eglLib) {
        if (void* p = dlsym(eglLib, s)) return p;
      }
      return dlsym(RTLD_DEFAULT, s);
    };
    eglGetDisplay = reinterpret_cast<EglGetDisplayFn>(symEgl("eglGetDisplay"));
    eglInitialize = reinterpret_cast<EglInitFn>(symEgl("eglInitialize"));
    eglChooseConfig = reinterpret_cast<EglChooseConfigFn>(symEgl("eglChooseConfig"));
    eglCreatePbufferSurface = reinterpret_cast<EglCreatePbufferSurfaceFn>(symEgl("eglCreatePbufferSurface"));
    eglCreateWindowSurface = reinterpret_cast<EglCreateWindowSurfaceFn>(symEgl("eglCreateWindowSurface"));
    eglBindAPI = reinterpret_cast<EglBindApiFn>(symEgl("eglBindAPI"));
    eglCreateContext = reinterpret_cast<EglCreateContextFn>(symEgl("eglCreateContext"));
    eglMakeCurrent = reinterpret_cast<EglMakeCurrentFn>(symEgl("eglMakeCurrent"));
    eglSwapBuffers = reinterpret_cast<EglSwapBuffersFn>(symEgl("eglSwapBuffers"));
    eglGetCurrentContext = reinterpret_cast<EglGetCurrentContextFn>(symEgl("eglGetCurrentContext"));
    eglGetConfigAttrib = reinterpret_cast<EglGetConfigAttribFn>(symEgl("eglGetConfigAttrib"));
    eglGetError = reinterpret_cast<EglGetErrorFn>(symEgl("eglGetError"));
  }

  void* resolveGl(const char* name) const {
    if (glesLib) {
      if (void* p = dlsym(glesLib, name)) return p;
    }
    return dlsym(RTLD_DEFAULT, name);
  }

  bool hasCurrentHardwareContext() const {
    return eglGetCurrentContext && eglGetCurrentContext() != nullptr;
  }
};

NativeGles& nativeGles() {
  static NativeGles n;
  return n;
}

void reportGraphicsIssue(const char* stage, int error) {
  g_lastGraphicsStage.store(stage, std::memory_order_relaxed);
  g_lastGraphicsError.store(error, std::memory_order_relaxed);
  char message[192];
  std::snprintf(message, sizeof message, "graphics setup issue at %s (code 0x%x)",
                stage ? stage : "unknown stage", static_cast<unsigned int>(error));
  logRunEvent(message);
}

void reportEglFailure(const char* stage) {
  NativeGles& ng = nativeGles();
  reportGraphicsIssue(stage, ng.eglGetError ? ng.eglGetError() : 0);
}

bool configureNativeWindowForEgl(void* display, void* window, void* config) {
#ifdef __ANDROID__
  if (!window || !config) return false;
  NativeGles& ng = nativeGles();
  if (!ng.eglGetConfigAttrib) {
    reportGraphicsIssue("eglGetConfigAttrib unavailable", 0);
    return false;
  }
  int nativeVisual = 0;
  if (!ng.eglGetConfigAttrib(display, config, 0x302E /* EGL_NATIVE_VISUAL_ID */, &nativeVisual)) {
    reportEglFailure("eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)");
    return false;
  }
  int status = ANativeWindow_setBuffersGeometry(static_cast<ANativeWindow*>(window), 0, 0, nativeVisual);
  if (status != 0) reportGraphicsIssue("ANativeWindow_setBuffersGeometry", status);
  return status == 0;
#else
  (void)display;
  (void)window;
  (void)config;
  return true;
#endif
}

void* g_eglConfig = nullptr;  // chosen EGLConfig for surface (re)creation

void ensureEglContextInitialized(DummyEAGLContext* ctx) {
  if (!ctx || ctx->eglContext != 0) return;
  NativeGles& ng = nativeGles();
  if (!ng.eglGetDisplay || !ng.eglInitialize || !ng.eglChooseConfig || !ng.eglCreateContext) {
    reportGraphicsIssue("required EGL entry point unavailable", 0);
    return;
  }
  void* dpy = ng.eglGetDisplay(nullptr);  // EGL_DEFAULT_DISPLAY
  if (!dpy) { reportEglFailure("eglGetDisplay"); return; }
  int major = 0, minor = 0;
  if (!ng.eglInitialize(dpy, &major, &minor)) { reportEglFailure("eglInitialize"); return; }
  if (ng.eglBindAPI) ng.eglBindAPI(0x30A0);  // EGL_OPENGL_ES_API
  // EGL_RENDERABLE_TYPE=EGL_OPENGL_ES2_BIT(4), EGL_SURFACE_TYPE=EGL_PBUFFER_BIT|EGL_WINDOW_BIT(5),
  // EGL_RED_SIZE=8, EGL_GREEN_SIZE=8, EGL_BLUE_SIZE=8, EGL_DEPTH_SIZE=16, EGL_NONE(0x3038)
  const int configAttribs[] = {
      0x3040, 0x0004,
      0x3033, 0x0005,
      0x3024, 8,
      0x3023, 8,
      0x3022, 8,
      0x3025, 16,
      0x3038
  };
  void* cfg = nullptr;
  int numCfg = 0;
  if (!ng.eglChooseConfig(dpy, configAttribs, &cfg, 1, &numCfg) || numCfg < 1) {
    reportEglFailure("eglChooseConfig");
    return;
  }
  // EGL_CONTEXT_CLIENT_VERSION (0x3098), 2, EGL_NONE (0x3038)
  const int ctxAttribs[] = {0x3098, 2, 0x3038};
  void* eglCtx = ng.eglCreateContext(dpy, cfg, nullptr, ctxAttribs);
  if (!eglCtx) { reportEglFailure("eglCreateContext(OpenGL ES 2)"); return; }
  void* surf = nullptr;
  g_eglWindowSurface = false;
  // Match the ANativeWindow buffer format to the selected EGLConfig before creating a window
  // surface. Without this, some Android devices reject the surface and silently fall back to a
  // pbuffer, which makes every successful swap invisible to the user.
  if (g_androidNativeWindow && ng.eglCreateWindowSurface) {
    configureNativeWindowForEgl(dpy, g_androidNativeWindow, cfg);
    const int winAttribs[] = {0x3038};
    surf = ng.eglCreateWindowSurface(dpy, cfg, g_androidNativeWindow, winAttribs);
    if (surf) g_eglWindowSurface = true;
    else reportEglFailure("eglCreateWindowSurface");
  }
  if (!surf && ng.eglCreatePbufferSurface) {
    // EGL_WIDTH, g_surfaceWidth, EGL_HEIGHT, g_surfaceHeight, EGL_NONE
    const int pbufAttribs[] = {0x3057, g_surfaceWidth, 0x3056, g_surfaceHeight, 0x3038};
    surf = ng.eglCreatePbufferSurface(dpy, cfg, pbufAttribs);
    if (!surf) reportEglFailure("eglCreatePbufferSurface");
    else if (g_androidNativeWindow) logRunEvent("EGL using a pbuffer fallback; frames will not be visible on the Android Surface");
  }
  ctx->eglDisplay = reinterpret_cast<uint64_t>(dpy);
  ctx->eglSurface = reinterpret_cast<uint64_t>(surf);
  ctx->eglContext = reinterpret_cast<uint64_t>(eglCtx);
  // Store the chosen EGLConfig at offset slot we don't currently use: stash in a static so
  // surface recreation can reuse it.
  g_eglConfig = cfg;
}

// Result of one present attempt through presentEglFrameLocked().
enum class EglPresentResult {
  kSwapped,       // eglSwapBuffers succeeded; presentedFrames was incremented
  kNoEgl,         // no EGL entry points / display / surface available
  kRebindFailed,  // the context could not be made current on this thread first
  kSwapFailed,    // eglSwapBuffers itself failed
};

// Presents the context's current EGL surface. Must hold g_bridgeMutex and be called on the
// render thread. Before eglSwapBuffers, eglGetCurrentContext() is compared against the context
// we intend to present with; on mismatch (foreign or no context current on this thread) we
// explicitly unbind with eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)
// and rebind ours -- rebinding without that step raises EGL_BAD_ACCESS (0x3002).
EglPresentResult presentEglFrameLocked(DummyEAGLContext* ctx) {
  if (!ctx) return EglPresentResult::kNoEgl;
  NativeGles& ng = nativeGles();
  if (!ng.eglSwapBuffers || !ng.eglMakeCurrent || !ctx->eglDisplay || !ctx->eglSurface)
    return EglPresentResult::kNoEgl;
  void* dpy = reinterpret_cast<void*>(ctx->eglDisplay);
  void* surf = reinterpret_cast<void*>(ctx->eglSurface);
  void* want = reinterpret_cast<void*>(ctx->eglContext);
  if (ng.eglGetCurrentContext) {
    void* current = ng.eglGetCurrentContext();
    if (current != want) {
      // Explicit EGL_NO_SURFACE / EGL_NO_CONTEXT unbind before rebinding (0x3002 avoidance).
      if (current && !ng.eglMakeCurrent(dpy, nullptr, nullptr, nullptr))
        reportEglFailure("eglMakeCurrent(EGL_NO_SURFACE, EGL_NO_CONTEXT) unbind");
      if (!ng.eglMakeCurrent(dpy, surf, surf, want)) {
        reportEglFailure("eglMakeCurrent (present rebind)");
        ctx->isCurrent = 0;
        return EglPresentResult::kRebindFailed;
      }
      ctx->isCurrent = 1;
      g_eglOwnerTid = hostThreadId();
    }
  }
  if (!ng.eglSwapBuffers(dpy, surf)) return EglPresentResult::kSwapFailed;
  ++ctx->presentedFrames;
  static std::atomic<bool> firstSwapLogged{false};
  if (!firstSwapLogged.exchange(true, std::memory_order_relaxed))
    logRunEvent("first frame presented to the Android Surface via eglSwapBuffers (successfulSwaps=1)");
  return EglPresentResult::kSwapped;
}

// Interned string storage for dynamic ObjC string operations (stringByAppendingPathComponent:, etc.)
std::mutex g_strPoolMutex;
std::deque<std::string> g_strPool;
const char* internDynamicString(std::string s) {
  std::lock_guard<std::mutex> lock(g_strPoolMutex);
  for (const auto& existing : g_strPool) {
    if (existing == s) return existing.c_str();
  }
  g_strPool.push_back(std::move(s));
  return g_strPool.back().c_str();
}

// Safe check for whether a 64-bit value looks like a mapped user-space pointer rather than a
// small synthetic integer (such as 0x1234 or 0xDEAD in unit tests).
bool looksLikeReadablePointer(uint64_t p) {
  if (p < 0x100000ull || p > 0x00007FFFFFFFFFFFull) return false;
  return true;
}

}  // namespace

DummyEAGLContext* eaglDefaultContext() {
  if (g_dummyEAGLContext.api == 0) g_dummyEAGLContext.api = 2;
  if (g_dummyEAGLContext.sharegroup == 0) {
    g_dummyEAGLContext.sharegroup = reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub);
  }
  return &g_dummyEAGLContext;
}

DummyEAGLContext* eaglInitWithAPI(void* self, uint32_t api) {
  noteCompatCall("EAGLContext::initWithAPI");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  DummyEAGLContext* ctx = &g_dummyEAGLContext;
  if (self == &g_dummyEAGLContext) ctx = static_cast<DummyEAGLContext*>(self);
  ctx->api = api ? api : 2;
  ctx->sharegroup = reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub);
  ensureEglContextInitialized(ctx);
  return ctx;
}

int eaglSetCurrentContext(void* ctxPtr) {
  noteCompatCall("EAGLContext::setCurrentContext");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  if (!ctxPtr) {
    if (g_currentEAGLContext) g_currentEAGLContext->isCurrent = 0;
    g_currentEAGLContext = nullptr;
    NativeGles& ng = nativeGles();
    bool bindingReleased = false;
    if (ng.eglMakeCurrent && g_dummyEAGLContext.eglDisplay) {
      void* dpy = reinterpret_cast<void*>(g_dummyEAGLContext.eglDisplay);
      // Only this thread may release the binding when it actually holds it; unbinding from a
      // different thread while the render thread owns the context raises EGL_BAD_ACCESS (0x3002).
      void* current = ng.eglGetCurrentContext ? ng.eglGetCurrentContext() : nullptr;
      if (current == reinterpret_cast<void*>(g_dummyEAGLContext.eglContext)) {
        ng.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
        bindingReleased = true;
      } else if (!current && !eglBindingOwnedByOtherThreadLocked()) {
        bindingReleased = true;  // nothing bound anywhere; stale bookkeeping only
      }
    } else if (!eglBindingOwnedByOtherThreadLocked()) {
      bindingReleased = true;  // no EGL present: bookkeeping only, never steal the render thread
    }
    if (bindingReleased) g_eglOwnerTid = 0;
    return 1;
  }
  DummyEAGLContext* ctx = (ctxPtr == &g_dummyEAGLContext) ? static_cast<DummyEAGLContext*>(ctxPtr) : &g_dummyEAGLContext;
  if (ctx->api == 0) ctx->api = 2;
  if (ctx->sharegroup == 0) ctx->sharegroup = reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub);
  ensureEglContextInitialized(ctx);
  NativeGles& ng = nativeGles();
  bool madeCurrent = false;
  if (ng.eglMakeCurrent && ctx->eglDisplay && ctx->eglContext && ctx->eglSurface) {
    void* dpy = reinterpret_cast<void*>(ctx->eglDisplay);
    void* surf = reinterpret_cast<void*>(ctx->eglSurface);
    void* want = reinterpret_cast<void*>(ctx->eglContext);
    // If a different context is current on this thread, explicitly unbind it first
    // (eglMakeCurrent would otherwise fail with EGL_BAD_ACCESS 0x3002).
    if (ng.eglGetCurrentContext) {
      void* current = ng.eglGetCurrentContext();
      if (current && current != want) ng.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
    }
    madeCurrent = ng.eglMakeCurrent(dpy, surf, surf, want) != 0;
    if (!madeCurrent) reportEglFailure("eglMakeCurrent");
  } else {
    reportGraphicsIssue("EGL context/surface unavailable when setting current", 0);
  }
  ctx->isCurrent = madeCurrent ? 1u : 0u;
  // The caller designates itself as the render thread; queued surface rebinds drain here and
  // the next present attempt retries the binding (self-healing when EGL was not ready yet).
  g_eglOwnerTid = hostThreadId();
  g_currentEAGLContext = ctx;
  // Preserve the old compatibility behavior (UIKit reports success) while the run log and
  // live diagnostics expose whether a real Android EGL context was actually made current.
  return 1;
}

DummyEAGLContext* eaglGetCurrentContext() {
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  return g_currentEAGLContext ? g_currentEAGLContext : &g_dummyEAGLContext;
}

int eaglPresentRenderbuffer(void* self, uint32_t /*target*/) {
  noteCompatCall("EAGLContext::presentRenderbuffer");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  DummyEAGLContext* ctx = (self == &g_dummyEAGLContext) ? static_cast<DummyEAGLContext*>(self) : &g_dummyEAGLContext;
  switch (presentEglFrameLocked(ctx)) {
    case EglPresentResult::kNoEgl: {
      static std::atomic<uint64_t> missingSurfaceWarnings{0};
      uint64_t count = missingSurfaceWarnings.fetch_add(1, std::memory_order_relaxed) + 1;
      if (count == 1 || count % 300 == 0)
        reportGraphicsIssue("presentRenderbuffer has no EGL surface", 0);
      break;
    }
    case EglPresentResult::kSwapFailed: {
      static std::atomic<uint64_t> swapFailures{0};
      uint64_t count = swapFailures.fetch_add(1, std::memory_order_relaxed) + 1;
      if (count == 1 || count % 300 == 0) reportEglFailure("eglSwapBuffers");
      break;
    }
    default:
      break;  // swapped (or a rebind failure already reported by the helper)
  }
  return 1;
}

int eaglRenderbufferStorageFromDrawable(void* /*self*/, uint32_t target, void* /*drawable*/) {
  noteCompatCall("EAGLContext::renderbufferStorage:fromDrawable:");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using GlRenderbufferStorageFn = void (*)(uint32_t, uint32_t, int, int);
    static auto fn = reinterpret_cast<GlRenderbufferStorageFn>(ng.resolveGl("glRenderbufferStorage"));
    if (fn) {
      // GL_RGBA8_OES (0x8058) or GL_RGB565 (0x8D62)
      fn(target ? target : 0x8D41, 0x8D62, g_surfaceWidth, g_surfaceHeight);
    }
  }
  return 1;
}

// (Re)creates the EGL window surface for the currently bound dummy EAGLContext. Must be called
// under g_bridgeMutex AND on the thread that owns the EGL binding (the render thread); callers
// on any other thread must defer via g_eglRebindPending instead (see bindAndroidEglWindow /
// updateAndroidEglWindow), because eglMakeCurrent from a foreign thread raises EGL_BAD_ACCESS
// (0x3002) while the render thread holds the context current.
void recreateEglSurfaceLocked() {
  NativeGles& ng = nativeGles();
  if (!g_dummyEAGLContext.eglDisplay || !g_dummyEAGLContext.eglContext || !g_eglConfig) return;
  void* dpy = reinterpret_cast<void*>(g_dummyEAGLContext.eglDisplay);
  void* want = reinterpret_cast<void*>(g_dummyEAGLContext.eglContext);
  // A current EGLSurface cannot be destroyed: first compare eglGetCurrentContext() with the
  // context we manage and explicitly unbind (EGL_NO_SURFACE / EGL_NO_CONTEXT) before the swap.
  if (g_dummyEAGLContext.eglSurface && ng.eglMakeCurrent && ng.eglGetCurrentContext) {
    void* current = ng.eglGetCurrentContext();
    if (current == want) {
      ng.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
      g_eglOwnerTid = hostThreadId();  // this thread still owns the (now unbound) context
    } else if (current) {
      // A foreign context is current on this thread; unbind it explicitly so the following
      // eglMakeCurrent for our context cannot fail with EGL_BAD_ACCESS (0x3002).
      ng.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
    }
  }
  if (g_dummyEAGLContext.eglSurface) {
    using EglDestroySurfaceFn = int (*)(void*, void*);
    static auto destroy = reinterpret_cast<EglDestroySurfaceFn>(dlsym(ng.eglLib ? ng.eglLib : RTLD_DEFAULT, "eglDestroySurface"));
    if (destroy) destroy(dpy, reinterpret_cast<void*>(g_dummyEAGLContext.eglSurface));
    g_dummyEAGLContext.eglSurface = 0;
  }
  void* surf = nullptr;
  g_eglWindowSurface = false;
  if (g_androidNativeWindow && ng.eglCreateWindowSurface) {
    configureNativeWindowForEgl(dpy, g_androidNativeWindow, g_eglConfig);
    const int winAttribs[] = {0x3038};
    surf = ng.eglCreateWindowSurface(dpy, g_eglConfig, g_androidNativeWindow, winAttribs);
    if (surf) g_eglWindowSurface = true;
    else reportEglFailure("eglCreateWindowSurface (rebind)");
  }
  if (!surf && ng.eglCreatePbufferSurface) {
    const int pbufAttribs[] = {0x3057, g_surfaceWidth, 0x3056, g_surfaceHeight, 0x3038};
    surf = ng.eglCreatePbufferSurface(dpy, g_eglConfig, pbufAttribs);
    if (!surf) reportEglFailure("eglCreatePbufferSurface (rebind)");
    else if (g_androidNativeWindow) logRunEvent("EGL surface rebind fell back to a pbuffer; game frames will not reach the screen");
  }
  g_dummyEAGLContext.eglSurface = reinterpret_cast<uint64_t>(surf);
  if (g_dummyEAGLContext.isCurrent && surf && ng.eglMakeCurrent) {
    if (ng.eglMakeCurrent(dpy, surf, surf, want)) {
      g_eglOwnerTid = hostThreadId();
    } else {
      reportEglFailure("eglMakeCurrent (surface rebind)");
      g_dummyEAGLContext.isCurrent = 0;
      g_eglOwnerTid = 0;
    }
  }
}

void bindAndroidEglWindow(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  void* previous = g_androidNativeWindow;
  const bool sameWindow = previous == nativeWindow;
  if (!sameWindow) g_androidNativeWindow = nativeWindow;
  if (width > 0) g_surfaceWidth = width;
  if (height > 0) g_surfaceHeight = height;
  g_dummyUIWindow.width = static_cast<uint32_t>(g_surfaceWidth);
  g_dummyUIWindow.height = static_cast<uint32_t>(g_surfaceHeight);
  // Ensure EGL context exists and attach a window surface to it.
  ensureEglContextInitialized(&g_dummyEAGLContext);
  if (eglBindingOwnedByOtherThreadLocked()) {
    // The render thread holds the EGL binding: queue the rebind for it instead of touching
    // eglMakeCurrent here (that would raise EGL_BAD_ACCESS 0x3002 on the UI thread).
    g_eglRebindPending.store(true, std::memory_order_release);
    logRunEvent("EGL surface rebind deferred to the render thread after Surface creation (EGL context current elsewhere; avoiding EGL_BAD_ACCESS 0x3002)");
  } else if (g_dummyEAGLContext.eglDisplay) {
    recreateEglSurfaceLocked();
  }
#ifdef __ANDROID__
  // ANativeWindow_fromSurface returns an acquired reference. The bridge owns the current one;
  // release the redundant incoming reference for the same window, or the replaced window after
  // its EGLSurface has been destroyed.
  if (sameWindow && nativeWindow) ANativeWindow_release(static_cast<ANativeWindow*>(nativeWindow));
  else if (previous) ANativeWindow_release(static_cast<ANativeWindow*>(previous));
#endif
  logRunEvent(std::string("Android Surface bound: ") + std::to_string(g_surfaceWidth) + "x" +
              std::to_string(g_surfaceHeight) + (g_eglWindowSurface ? " (EGL window surface)" : " (no EGL window surface)"));
}

void updateAndroidEglWindow(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  void* previous = g_androidNativeWindow;
  const bool sameWindow = previous == nativeWindow;
  if (!sameWindow) g_androidNativeWindow = nativeWindow;
  if (width > 0) g_surfaceWidth = width;
  if (height > 0) g_surfaceHeight = height;
  g_dummyUIWindow.width = static_cast<uint32_t>(g_surfaceWidth);
  g_dummyUIWindow.height = static_cast<uint32_t>(g_surfaceHeight);
  if (eglBindingOwnedByOtherThreadLocked()) {
    // Android Surface changed on a non-render thread while the render thread holds the EGL
    // context current. Rebuilding the surface here would call eglMakeCurrent on this thread
    // and fail with EGL_BAD_ACCESS (0x3002); queue the rebind for the render thread instead.
    g_eglRebindPending.store(true, std::memory_order_release);
    logRunEvent("EGL surface rebind deferred to the render thread after Android Surface changed (EGL context current elsewhere; avoiding EGL_BAD_ACCESS 0x3002)");
  } else if (g_dummyEAGLContext.eglDisplay) {
    recreateEglSurfaceLocked();
  }
#ifdef __ANDROID__
  // The new window (or null) is already recorded above; the outgoing window's own reference is
  // released here. The old EGLSurface still holds its own ANativeWindow reference, so it stays
  // alive until the deferred rebind destroys it on the render thread.
  if (sameWindow && nativeWindow) ANativeWindow_release(static_cast<ANativeWindow*>(nativeWindow));
  else if (previous) ANativeWindow_release(static_cast<ANativeWindow*>(previous));
#endif
  logRunEvent(std::string("Android Surface changed: ") + std::to_string(g_surfaceWidth) + "x" +
              std::to_string(g_surfaceHeight) + (g_eglWindowSurface ? " (EGL window surface)" : " (no EGL window surface)"));
}

void eglDrawableSize(int* outWidth, int* outHeight) {
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  if (outWidth) *outWidth = g_surfaceWidth;
  if (outHeight) *outHeight = g_surfaceHeight;
}

GraphicsStatus currentGraphicsStatus() {
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  GraphicsStatus status;
  status.eventLoopActive = g_uiLoopActive.load(std::memory_order_acquire);
  status.nativeWindowBound = g_androidNativeWindow != nullptr;
  status.eglReady = g_dummyEAGLContext.eglDisplay && g_dummyEAGLContext.eglContext && g_dummyEAGLContext.eglSurface;
  status.windowSurface = g_eglWindowSurface;
  status.contextCurrent = g_dummyEAGLContext.isCurrent != 0;
  status.displayLinkRegistered = g_dummyCADisplayLink.registeredInRunLoop != 0;
  status.surfaceRebindPending = g_eglRebindPending.load(std::memory_order_acquire);
  status.width = g_surfaceWidth;
  status.height = g_surfaceHeight;
  status.heartbeatFps = g_heartbeatFps.load(std::memory_order_relaxed);
  status.uiFrames = g_frameCounter.load(std::memory_order_relaxed);
  status.presentedFrames = g_dummyEAGLContext.presentedFrames;
  status.engineForcedSwaps = g_engineForcedSwaps.load(std::memory_order_relaxed);
  const char* stage = g_lastGraphicsStage.load(std::memory_order_relaxed);
  const int error = g_lastGraphicsError.load(std::memory_order_relaxed);
  if (stage) status.lastEglIssue = std::string(stage) + " (0x" + [] (int value) {
    char hex[16]; std::snprintf(hex, sizeof hex, "%x", static_cast<unsigned int>(value)); return std::string(hex);
  }(error) + ")";
  return status;
}

void requestExitUiLoop(int exitCode) {
  g_uiLoopExitCode.store(exitCode, std::memory_order_relaxed);
  g_uiLoopShouldExit.store(true, std::memory_order_release);
  touchQueue().cv.notify_all();
}

void injectTouchEvent(int action, float x, float y, int64_t timestampMs) {
  int phase;
  switch (action) {
    case 0: phase = 0; g_activePointerId.store(0); break;  // UITouchPhaseBegan
    case 1: phase = 3; break;                               // UITouchPhaseEnded
    case 2: phase = 1; break;                               // UITouchPhaseMoved
    default: phase = 4; break;                              // UITouchPhaseCancelled
  }
  g_lastTouchX.store(x, std::memory_order_relaxed);
  g_lastTouchY.store(y, std::memory_order_relaxed);
  g_lastTouchAction.store(phase, std::memory_order_relaxed);
  // Update the global dummy UITouch / UIEvent the guest will read via [UIEvent allTouches].
  {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    g_dummyUITouch.x = x;
    g_dummyUITouch.y = y;
    g_dummyUITouch.phase = phase;
    g_dummyUITouch.timestamp = static_cast<double>(timestampMs) * 1e-3;
    g_dummyUIEvent.touch = g_dummyUITouch;
  }
  touchQueue().push(TouchEvent{action, x, y, timestampMs, 0});
}

DummyUIWindow* uiWindowDefault() { return &g_dummyUIWindow; }

int uiWindowMakeKeyAndVisible(void* window) {
  noteCompatCall("UIWindow::makeKeyAndVisible");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  DummyUIWindow* w = (window == &g_dummyUIWindow) ? static_cast<DummyUIWindow*>(window) : &g_dummyUIWindow;
  w->keyAndVisible = 1;
  return 1;
}

DummyCADisplayLink* caDisplayLinkDefault() { return &g_dummyCADisplayLink; }

DummyCADisplayLink* caDisplayLinkWithTargetSelector(uint64_t target, uint64_t selector) {
  noteCompatCall("CADisplayLink::displayLinkWithTarget:selector:");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  g_dummyCADisplayLink.target = target;
  g_dummyCADisplayLink.selector = selector;
  g_dummyCADisplayLink.frameInterval = 1;
  g_dummyCADisplayLink.duration = 1.0 / 60.0;
  char msg[128];
  std::snprintf(msg, sizeof msg, "CADisplayLink registered: target=0x%llx selector=0x%llx (60 FPS heartbeat)",
                static_cast<unsigned long long>(target), static_cast<unsigned long long>(selector));
  logRunEvent(msg);
  return &g_dummyCADisplayLink;
}

int caDisplayLinkAddToRunLoop(void* link, uint64_t runLoop, uint64_t mode) {
  noteCompatCall("CADisplayLink::addToRunLoop:forMode:");
  std::lock_guard<std::mutex> lock(g_bridgeMutex);
  DummyCADisplayLink* dl = (link == &g_dummyCADisplayLink) ? static_cast<DummyCADisplayLink*>(link) : &g_dummyCADisplayLink;
  dl->runLoop = runLoop;
  dl->mode = mode;
  dl->registeredInRunLoop = 1;
  // The UIApplicationMain frame loop picks the registration up on its next cycle (the target
  // and selector are re-read every frame), so a display link created after launch still drives
  // the guest render step immediately.
  logRunEvent("CADisplayLink added to the run loop; the engine frame heartbeat will invoke its target selector every frame cycle");
  return 1;
}

namespace {

// --- written shims -------------------------------------------------------------------------
// AAPCS64: extra register/stack arguments the shims do not name are simply ignored. Floats
// arrive in d-registers the shims never read. Structs-by-value the shims never consume.

void* g_dummyPoolToken = &g_dummyPoolToken;  // any stable non-null pointer is a legal "token"
const char g_emptyString[] = "";

uint64_t passthrough(uint64_t v) { return v; }                    // objc retain family
void voidShim() {}                                                // fire-and-forget calls
void voidShim1(uint64_t) {}
void voidShim2(uint64_t, uint64_t) {}
uint64_t zeroShim() { return 0; }                                 // nil / NULL returns
uint64_t emptyStringShim() { return reinterpret_cast<uint64_t>(g_emptyString); }
uint64_t poolPushShim() { return reinterpret_cast<uint64_t>(g_dummyPoolToken); }
double zeroDoubleShim() { return 0.0; }

// Selector-aware objc_msgSend shim: bridges EAGLContext, UIWindow, CADisplayLink, and
// Foundation path/string selectors while preserving x0 pass-through for everything else.
uint64_t objcMsgSendShim(uint64_t self, uint64_t sel, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  (void)a2;
  (void)a3;
  if (!looksLikeReadablePointer(sel)) return self;
  const char* s = reinterpret_cast<const char*>(sel);
  // Quickly validate first byte is printable identifier character.
  if (s[0] < 'A' || s[0] > 'z') return self;

  // --- EAGLContext methods ---
  if (std::strcmp(s, "initWithAPI:") == 0 || std::strcmp(s, "initWithAPI:sharegroup:") == 0) {
    return reinterpret_cast<uint64_t>(eaglInitWithAPI(reinterpret_cast<void*>(self), static_cast<uint32_t>(a0)));
  }
  if (std::strcmp(s, "setCurrentContext:") == 0) {
    return static_cast<uint64_t>(eaglSetCurrentContext(reinterpret_cast<void*>(a0)));
  }
  if (std::strcmp(s, "currentContext") == 0) {
    return reinterpret_cast<uint64_t>(eaglGetCurrentContext());
  }
  if (std::strcmp(s, "presentRenderbuffer:") == 0) {
    return static_cast<uint64_t>(eaglPresentRenderbuffer(reinterpret_cast<void*>(self), static_cast<uint32_t>(a0)));
  }
  if (std::strcmp(s, "renderbufferStorage:fromDrawable:") == 0) {
    return static_cast<uint64_t>(
        eaglRenderbufferStorageFromDrawable(reinterpret_cast<void*>(self), static_cast<uint32_t>(a0),
                                            reinterpret_cast<void*>(a1)));
  }
  if (std::strcmp(s, "sharegroup") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub);
  }
  if (std::strcmp(s, "API") == 0) {
    return g_dummyEAGLContext.api ? g_dummyEAGLContext.api : 2;
  }

  // --- UIWindow / UIScreen / UIView methods ---
  if (std::strcmp(s, "makeKeyAndVisible") == 0) {
    uiWindowMakeKeyAndVisible(reinterpret_cast<void*>(self));
    return self ? self : reinterpret_cast<uint64_t>(&g_dummyUIWindow.classStub);
  }
  if (std::strcmp(s, "setRootViewController:") == 0) {
    g_dummyUIWindow.rootViewController = a0;
    return self;
  }
  if (std::strcmp(s, "rootViewController") == 0) {
    return g_dummyUIWindow.rootViewController;
  }
  if (std::strcmp(s, "screen") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyUIScreen);
  }
  if (std::strcmp(s, "bounds") == 0 || std::strcmp(s, "applicationFrame") == 0) {
    // Return a pointer to a CGRect sized to the current window in points (width/scale).
    // MCPE reads [UIScreen mainScreen].bounds.size to choose the render target size.
    static double rect[4] = {};
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    rect[0] = 0; rect[1] = 0;
    rect[2] = static_cast<double>(g_surfaceWidth) / g_dummyUIWindow.scale;
    rect[3] = static_cast<double>(g_surfaceHeight) / g_dummyUIWindow.scale;
    return reinterpret_cast<uint64_t>(rect);
  }
  if (std::strcmp(s, "scale") == 0) {
    return g_dummyUIWindow.scale;
  }
  if (std::strcmp(s, "mainScreen") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyUIScreen);
  }
  if (std::strcmp(s, "keyWindow") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyUIWindow);
  }
  if (std::strcmp(s, "delegate") == 0) {
    return g_dummyUIApplication.delegate;
  }
  if (std::strcmp(s, "sharedApplication") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyUIApplication);
  }
  if (std::strcmp(s, "setIdleTimerDisabled:") == 0) {
    g_dummyUIApplication.idleTimerDisabled = static_cast<uint32_t>(a0 != 0);
    return self;
  }

  // --- UITouch / UIEvent / NSSet methods (Android MotionEvent -> iOS UITouch forwarding) ---
  if (std::strcmp(s, "allTouches") == 0 || std::strcmp(s, "touchesForView:") == 0 ||
      std::strcmp(s, "touchesForWindow:") == 0) {
    // Return a pointer that acts as an NSSet*; subsequent "anyObject"/"allObjects" on it works
    // because we route those selectors below to return the dummy touch.
    return reinterpret_cast<uint64_t>(&g_dummyUIEvent.touch);
  }
  if (std::strcmp(s, "anyObject") == 0) {
    return reinterpret_cast<uint64_t>(&g_dummyUITouch);
  }
  if (std::strcmp(s, "allObjects") == 0) {
    // Pretend an NSArray*; objectAtIndex: already returns self from the generic array path,
    // so returning the touch pointer gives callers a single-touch array they can index into.
    return reinterpret_cast<uint64_t>(&g_dummyUITouch);
  }
  if (std::strcmp(s, "phase") == 0) {
    return static_cast<uint64_t>(g_dummyUITouch.phase);
  }
  if (std::strcmp(s, "timestamp") == 0) {
    // Returning an integer/double via x0 for a double-returning selector is wrong under the hard
    // float ABI, but MCPE only uses phase and locationInView:; provide a best-effort seconds value.
    union { double d; uint64_t u; } c;
    c.d = g_dummyUITouch.timestamp;
    return c.u;
  }
  if (std::strcmp(s, "locationInView:") == 0) {
    // Return a CGPoint* to the guest's requested location in pixels; MCPE converts to points.
    static double pt[2];
    float x = g_lastTouchX.load(std::memory_order_relaxed);
    float y = g_lastTouchY.load(std::memory_order_relaxed);
    pt[0] = static_cast<double>(x);
    pt[1] = static_cast<double>(y);
    return reinterpret_cast<uint64_t>(pt);
  }
  if (std::strcmp(s, "contentScaleFactor") == 0) {
    union { double d; uint64_t u; } c;
    c.d = g_dummyUIWindow.scale;
    return c.u;
  }

  // --- CADisplayLink methods ---
  if (std::strcmp(s, "displayLinkWithTarget:selector:") == 0) {
    return reinterpret_cast<uint64_t>(caDisplayLinkWithTargetSelector(a0, a1));
  }
  if (std::strcmp(s, "addToRunLoop:forMode:") == 0) {
    caDisplayLinkAddToRunLoop(reinterpret_cast<void*>(self), a0, a1);
    return self;
  }
  if (std::strcmp(s, "setFrameInterval:") == 0) {
    g_dummyCADisplayLink.frameInterval = static_cast<uint32_t>(a0 ? a0 : 1);
    return self;
  }
  if (std::strcmp(s, "setPaused:") == 0) {
    g_dummyCADisplayLink.paused = static_cast<uint32_t>(a0 != 0);
    return self;
  }
  if (std::strcmp(s, "invalidate") == 0) {
    g_dummyCADisplayLink.registeredInRunLoop = 0;
    return self;
  }

  // --- Foundation path / NSString / NSArray / NSBundle / NSFileManager methods ---
  if (std::strcmp(s, "UTF8String") == 0 ||
      std::strcmp(s, "fileSystemRepresentation") == 0 ||
      std::strcmp(s, "cStringUsingEncoding:") == 0) {
    if (looksLikeReadablePointer(self)) return self;
    return reinterpret_cast<uint64_t>(g_emptyString);
  }
  if (std::strcmp(s, "objectAtIndex:") == 0 ||
      std::strcmp(s, "firstObject") == 0 ||
      std::strcmp(s, "lastObject") == 0) {
    return self;
  }
  if (std::strcmp(s, "count") == 0) {
    return self ? 1 : 0;
  }
  if (std::strcmp(s, "stringWithUTF8String:") == 0 ||
      std::strcmp(s, "initWithUTF8String:") == 0 ||
      std::strcmp(s, "stringWithCString:encoding:") == 0) {
    if (looksLikeReadablePointer(a0)) {
      return reinterpret_cast<uint64_t>(internDynamicString(reinterpret_cast<const char*>(a0)));
    }
    return reinterpret_cast<uint64_t>(g_emptyString);
  }
  if (std::strcmp(s, "stringByAppendingPathComponent:") == 0) {
    std::string lhs = looksLikeReadablePointer(self) ? reinterpret_cast<const char*>(self) : "";
    std::string rhs = looksLikeReadablePointer(a0) ? reinterpret_cast<const char*>(a0) : "";
    if (!lhs.empty() && lhs.back() != '/' && !rhs.empty() && rhs.front() != '/') lhs += '/';
    return reinterpret_cast<uint64_t>(internDynamicString(lhs + rhs));
  }
  if (std::strcmp(s, "stringByAppendingString:") == 0) {
    std::string lhs = looksLikeReadablePointer(self) ? reinterpret_cast<const char*>(self) : "";
    std::string rhs = looksLikeReadablePointer(a0) ? reinterpret_cast<const char*>(a0) : "";
    return reinterpret_cast<uint64_t>(internDynamicString(lhs + rhs));
  }
  if (std::strcmp(s, "bundlePath") == 0 || std::strcmp(s, "resourcePath") == 0) {
    return reinterpret_cast<uint64_t>(guestNSHomeDirectory());
  }
  if (std::strcmp(s, "fileExistsAtPath:") == 0 ||
      std::strcmp(s, "fileExistsAtPath:isDirectory:") == 0) {
    if (!looksLikeReadablePointer(a0)) return 0;
    std::string hostPath = resolveSandboxHostPath(reinterpret_cast<const char*>(a0));
    struct stat st{};
    if (::stat(hostPath.c_str(), &st) != 0) return 0;
    if (std::strcmp(s, "fileExistsAtPath:isDirectory:") == 0 && looksLikeReadablePointer(a1)) {
      *reinterpret_cast<uint8_t*>(a1) = S_ISDIR(st.st_mode) ? 1 : 0;
    }
    return 1;
  }
  if (std::strcmp(s, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:") == 0) {
    if (!looksLikeReadablePointer(a0)) return 0;
    std::string hostPath = resolveSandboxHostPath(reinterpret_cast<const char*>(a0));
    std::error_code ec;
    std::filesystem::create_directories(hostPath, ec);
    return (!ec || std::filesystem::is_directory(hostPath, ec)) ? 1 : 0;
  }

  return self;
}

// Invoke the CADisplayLink target selector (tick: method) on the registered target. This is
// the iOS "heartbeat" MCPE uses to drive its frame cadence. The guest has wired up target/sel
// via displayLinkWithTarget:selector:; we call objc_msgSend(target, selector, displayLink)
// once per frame.
// Defined forward; objc_msgSend is our shim function already registered as _objc_msgSend. We
// reach it through its address by dlsym'ing our own shim to avoid circular declarations.
void drainTouchEventsForFrame() {
  auto events = touchQueue().drain();
  (void)events;
  // Actual delivery into the guest responder chain is handled by the objc_msgSend shim
  // lookups for [UIEvent allTouches] / [UITouch locationInView:] below; the events are kept in
  // a static "last event" slot that selectors can read.
}

// Returns the current guest time in seconds (matches CACurrentMediaTime / NSDate semantics).
double hostMediaTimeSeconds() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Forward-declaration of the objc_msgSend shim (defined earlier in this anonymous namespace)
// so uiApplicationMainShim can invoke the CADisplayLink -tick: callback each frame.
uint64_t objcMsgSendShim(uint64_t self, uint64_t sel, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

int uiApplicationMainShim(int argc, char** argv, const void* principal, const void* delegate) {
  (void)argc; (void)argv; (void)principal; (void)delegate;
  ensureSandboxDirectories();
  noteCompatCall("UIApplicationMain");
  appendGuestOutput("[radeki] UIApplicationMain: entering native event loop (EGL/GLESv2 display bound)\n");

  // Make sure the EGL context + surface exist before entering the loop so the guest's
  // renderbufferStorage/presentRenderbuffer calls can operate on a real drawable. This shim
  // runs on the guest's main thread, which from here on is also the EGL render thread.
  {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if (g_dummyEAGLContext.api == 0) g_dummyEAGLContext.api = 2;
    ensureEglContextInitialized(&g_dummyEAGLContext);
    g_eglRebindPending.store(false, std::memory_order_relaxed);  // stale flag from an earlier run
    if (g_androidNativeWindow && g_dummyEAGLContext.eglDisplay) {
      recreateEglSurfaceLocked();
    }
  }
  eaglSetCurrentContext(&g_dummyEAGLContext);
  g_dummyUIApplication.applicationState = 0;  // UIApplicationStateActive
  g_uiLoopShouldExit.store(false, std::memory_order_release);
  g_uiLoopExitCode.store(0, std::memory_order_relaxed);
  g_frameCounter.store(0, std::memory_order_relaxed);
  g_engineForcedSwaps.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    g_dummyEAGLContext.presentedFrames = 0;
  }
  g_uiLoopActive.store(true, std::memory_order_release);
  {
    GraphicsStatus status = currentGraphicsStatus();
    logRunEvent(std::string("UIApplicationMain entered: Surface=") +
        (status.nativeWindowBound ? "attached" : "missing") + ", EGL=" +
        (status.eglReady ? "ready" : "not ready") + ", EGL window surface=" +
        (status.windowSurface ? "yes" : "no") + ", display link=" +
        (status.displayLinkRegistered ? "registered" : "not registered") + ", size=" +
        std::to_string(status.width) + "x" + std::to_string(status.height));
  }

  // CADisplayLink / engine frame heartbeat. Target is 60 FPS; when the host cannot hold that
  // pace (a full second of consecutive missed deadlines) the heartbeat permanently falls back
  // to 30 FPS for this run. Each frame cycle:
  //   1. Apply any EGL surface rebind queued by an Android Surface change. This always happens
  //      on the render thread, so eglMakeCurrent never executes on a foreign thread and the
  //      rebind cannot fail with EGL_BAD_ACCESS (0x3002).
  //   2. Drain any pending Android MotionEvents.
  //   3. Fire the registered CADisplayLink target selector (the guest's main render step).
  //   4. Present the frame: eglSwapBuffers immediately after the render step completes, also
  //      when the guest did not call presentRenderbuffer itself.
  //   5. Respect requestExitUiLoop() so the JNI host can terminate on surface destroy.
  using Clock = std::chrono::steady_clock;
  constexpr int kBaseFps = 60;
  constexpr int kMissedDeadlinesBeforeFallback = 60;  // ~1 s behind before dropping to 30 FPS
  int fallbackMultiplier = 1;  // 1 = 60 FPS, 2 = 30 FPS fallback (one-way per run)
  int missedDeadlines = 0;
  auto nextFrame = Clock::now();
  uint64_t frames = 0;
  bool loggedFirstTick = false;
  while (!g_uiLoopShouldExit.load(std::memory_order_acquire)) {
    // 1. Deferred EGL surface rebind (Android Surface changed on another thread).
    if (g_eglRebindPending.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(g_bridgeMutex);
      g_eglRebindPending.store(false, std::memory_order_relaxed);
      if (g_dummyEAGLContext.eglDisplay) recreateEglSurfaceLocked();
      logRunEvent(std::string("queued EGL surface rebind applied on the render thread") +
                  (g_dummyEAGLContext.eglDisplay ? "" : " (no EGL display on this host)"));
    }

    drainTouchEventsForFrame();

    // 2. Update the CADisplayLink clock so the guest sees a monotonically increasing timestamp,
    //    and derive this frame's cadence from the guest's frameInterval (setFrameInterval:)
    //    multiplied by the 30 FPS fallback factor.
    int guestInterval = 1;
    {
      std::lock_guard<std::mutex> lock(g_bridgeMutex);
      guestInterval = g_dummyCADisplayLink.frameInterval > 0 ? g_dummyCADisplayLink.frameInterval : 1;
      g_dummyCADisplayLink.duration = (1.0 / 60.0) * static_cast<double>(guestInterval * fallbackMultiplier);
      g_dummyCADisplayLink.timestamp = hostMediaTimeSeconds();
    }
    g_heartbeatFps.store(static_cast<uint32_t>(kBaseFps / (guestInterval * fallbackMultiplier)),
                         std::memory_order_relaxed);

    uint64_t target = 0, sel = 0, swapsBefore = 0;
    int paused = 0, registered = 0;
    {
      std::lock_guard<std::mutex> lock(g_bridgeMutex);
      target = g_dummyCADisplayLink.target;
      sel = g_dummyCADisplayLink.selector;
      paused = g_dummyCADisplayLink.paused;
      registered = g_dummyCADisplayLink.registeredInRunLoop;
      swapsBefore = g_dummyEAGLContext.presentedFrames;
    }

    // 3. Engine frame heartbeat: invoke the registered CADisplayLink target selector every
    // frame cycle so the guest's main render step executes.
    const bool fired = registered && !paused && target && sel;
    if (fired) {
      objcMsgSendShim(target, sel, reinterpret_cast<uint64_t>(&g_dummyCADisplayLink), 0, 0, 0);
      if (!loggedFirstTick) {
        loggedFirstTick = true;
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "CADisplayLink frame callback first invoked (target=0x%llx, selector=0x%llx)",
                      static_cast<unsigned long long>(target), static_cast<unsigned long long>(sel));
        logRunEvent(msg);
      }
    }

    ++frames;
    g_frameCounter.store(frames, std::memory_order_relaxed);

    // 4. Frame swap: present immediately after the guest frame render step completes. When the
    // callback already presented via presentRenderbuffer, presentedFrames moved and we do not
    // swap twice; otherwise we swap here so the frame still reaches the Android screen and
    // successfulSwaps advances in the run log / RTLS heartbeat.
    if (fired) {
      std::lock_guard<std::mutex> lock(g_bridgeMutex);
      if (g_dummyEAGLContext.presentedFrames == swapsBefore) {
        if (presentEglFrameLocked(&g_dummyEAGLContext) == EglPresentResult::kSwapped) {
          g_engineForcedSwaps.fetch_add(1, std::memory_order_relaxed);
        } else {
          static std::atomic<uint64_t> forcedSwapFailures{0};
          uint64_t count = forcedSwapFailures.fetch_add(1, std::memory_order_relaxed) + 1;
          if (count == 1 || count % 300 == 0)
            logRunEvent("post-render-step eglSwapBuffers has no usable EGL surface yet (occurrence " +
                        std::to_string(count) + ")");
        }
      }
    }

    if (realtimeLoggingEnabled() && frames % 300 == 0) {
      GraphicsStatus status = currentGraphicsStatus();
      std::string heartbeat = "UIApplicationMain heartbeat: loopFrames=" + std::to_string(frames) +
          ", successfulSwaps=" + std::to_string(status.presentedFrames) +
          ", engineForcedSwaps=" + std::to_string(status.engineForcedSwaps) +
          ", heartbeatFps=" + std::to_string(status.heartbeatFps) +
          ", displayLink=" + (status.displayLinkRegistered ? "registered" : "missing") +
          ", EGLwindow=" + (status.windowSurface ? "yes" : "no") +
          ", contextCurrent=" + (status.contextCurrent ? "yes" : "no") +
          ", rebindPending=" + (status.surfaceRebindPending ? "yes" : "no");
      if (!status.lastEglIssue.empty()) heartbeat += ", lastGraphicsIssue=" + status.lastEglIssue;
      logRunEvent(heartbeat);
    }

    // 5. Pacing with the automatic 30 FPS fallback.
    const auto frameDuration = std::chrono::nanoseconds(
        (1000000000LL * guestInterval * fallbackMultiplier) / kBaseFps);
    nextFrame += frameDuration;
    auto now = Clock::now();
    if (nextFrame > now) {
      missedDeadlines = 0;
      std::this_thread::sleep_until(nextFrame);
    } else {
      // We fell behind; reset pace instead of spiralling.
      nextFrame = now;
      if (++missedDeadlines >= kMissedDeadlinesBeforeFallback && fallbackMultiplier == 1) {
        fallbackMultiplier = 2;
        missedDeadlines = 0;
        logRunEvent("engine frame heartbeat missed " + std::to_string(kMissedDeadlinesBeforeFallback) +
                    " consecutive 60 FPS deadlines; falling back to a 30 FPS heartbeat");
      }
    }
  }
  g_uiLoopActive.store(false, std::memory_order_release);
  // Release the EGL binding on the render thread that owns it, so a later Surface change or a
  // follow-up run in this same process cannot collide with a stale cross-thread binding
  // (the other 0x3002 source).
  {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if (g_eglOwnerTid == hostThreadId()) {
      NativeGles& ng = nativeGles();
      if (ng.eglMakeCurrent && ng.eglGetCurrentContext && g_dummyEAGLContext.eglDisplay &&
          ng.eglGetCurrentContext() == reinterpret_cast<void*>(g_dummyEAGLContext.eglContext)) {
        ng.eglMakeCurrent(reinterpret_cast<void*>(g_dummyEAGLContext.eglDisplay), nullptr, nullptr, nullptr);
      }
      g_eglOwnerTid = 0;
      g_dummyEAGLContext.isCurrent = 0;
    }
    g_eglRebindPending.store(false, std::memory_order_relaxed);
  }
  GraphicsStatus finalStatus = currentGraphicsStatus();
  logRunEvent("UIApplicationMain exited after " + std::to_string(frames) +
      " loop frames, " + std::to_string(finalStatus.presentedFrames) + " successful EGL swaps and " +
      std::to_string(finalStatus.engineForcedSwaps) + " engine-forced presents");
  appendGuestOutput("[radeki] UIApplicationMain: event loop exiting\n");
  return g_uiLoopExitCode.load(std::memory_order_relaxed);
}

void nsLogShim(const void* format, ...) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "[radeki-stub] NSLog(fmt=%p) -- format not parsed\n", format);
  noteCompatCall("NSLog");
  appendGuestOutput(buf);
}

[[noreturn]] void objcExceptionThrowShim(uint64_t exception) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "guest Objective-C exception at %p",
                reinterpret_cast<void*>(exception));
  abortGuest(134, buf);
}

// --- OpenGL ES 2.0 -> Android EGL/GLESv2 forwarding wrappers -------------------------------
// When an Android EGL/GLESv2 context is current, calls forward directly into libGLESv2.so.
// When no hardware context is bound (or on a headless test host), safe GLESv2 state responses
// are returned so engine initialization (shader compilation, buffer generation, glGetString)
// never dereferences a null pointer or loops on GL errors.

std::atomic<uint32_t> g_nextGlObjectId{1};

const uint8_t* glGetStringShim(uint32_t name) {
  noteCompatCall("glGetString");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = const uint8_t* (*)(uint32_t);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glGetString"));
    if (realFn) {
      if (const uint8_t* s = realFn(name)) return s;
    }
  }
  switch (name) {
    case 0x1F00: return reinterpret_cast<const uint8_t*>("RadekiOSNative");                  // GL_VENDOR
    case 0x1F01: return reinterpret_cast<const uint8_t*>("RadekiOSNative EGL/GLESv2 Bridge"); // GL_RENDERER
    case 0x1F02: return reinterpret_cast<const uint8_t*>("OpenGL ES 2.0 RadekiOSNative");     // GL_VERSION
    case 0x8B8C: return reinterpret_cast<const uint8_t*>("OpenGL ES GLSL ES 1.00");           // GL_SHADING_LANGUAGE_VERSION
    case 0x1F03: return reinterpret_cast<const uint8_t*>("GL_OES_vertex_array_object GL_OES_mapbuffer GL_OES_depth24 GL_OES_rgb8_rgba8 GL_EXT_discard_framebuffer GL_APPLE_framebuffer_multisample"); // GL_EXTENSIONS
    default: return reinterpret_cast<const uint8_t*>("");
  }
}

uint32_t glGetErrorShim() {
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = uint32_t (*)();
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glGetError"));
    if (realFn) return realFn();
  }
  return 0;  // GL_NO_ERROR
}

uint32_t glCheckFramebufferStatusShim(uint32_t target) {
  noteCompatCall("glCheckFramebufferStatus");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = uint32_t (*)(uint32_t);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glCheckFramebufferStatus"));
    if (realFn) {
      uint32_t st = realFn(target);
      if (st != 0) return st;
    }
  }
  return 0x8CD5;  // GL_FRAMEBUFFER_COMPLETE
}

void glGenObjectsHelper(const char* glSym, int n, uint32_t* ids) {
  if (n <= 0 || !ids) return;
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(int, uint32_t*);
    if (auto realFn = reinterpret_cast<Fn>(ng.resolveGl(glSym))) {
      ids[0] = 0;
      realFn(n, ids);
      if (ids[0] != 0) return;
    }
  }
  for (int i = 0; i < n; ++i) {
    ids[i] = g_nextGlObjectId.fetch_add(1, std::memory_order_relaxed);
  }
}

void glGenBuffersShim(int n, uint32_t* buffers) { noteCompatCall("glGenBuffers"); glGenObjectsHelper("glGenBuffers", n, buffers); }
void glGenTexturesShim(int n, uint32_t* textures) { noteCompatCall("glGenTextures"); glGenObjectsHelper("glGenTextures", n, textures); }
void glGenFramebuffersShim(int n, uint32_t* framebuffers) { noteCompatCall("glGenFramebuffers"); glGenObjectsHelper("glGenFramebuffers", n, framebuffers); }
void glGenRenderbuffersShim(int n, uint32_t* renderbuffers) { noteCompatCall("glGenRenderbuffers"); glGenObjectsHelper("glGenRenderbuffers", n, renderbuffers); }
void glGenVertexArraysOESShim(int n, uint32_t* arrays) { noteCompatCall("glGenVertexArraysOES"); glGenObjectsHelper("glGenVertexArraysOES", n, arrays); }

uint32_t glCreateShaderShim(uint32_t type) {
  noteCompatCall("glCreateShader");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = uint32_t (*)(uint32_t);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glCreateShader"));
    if (realFn) {
      if (uint32_t id = realFn(type)) return id;
    }
  }
  return g_nextGlObjectId.fetch_add(1, std::memory_order_relaxed);
}

uint32_t glCreateProgramShim() {
  noteCompatCall("glCreateProgram");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = uint32_t (*)();
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glCreateProgram"));
    if (realFn) {
      if (uint32_t id = realFn()) return id;
    }
  }
  return g_nextGlObjectId.fetch_add(1, std::memory_order_relaxed);
}

void glGetShaderivShim(uint32_t shader, uint32_t pname, int32_t* params) {
  if (!params) return;
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t, uint32_t, int32_t*);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glGetShaderiv"));
    if (realFn) {
      realFn(shader, pname, params);
      return;
    }
  }
  // GL_COMPILE_STATUS (0x8B81) -> GL_TRUE (1); GL_INFO_LOG_LENGTH (0x8B84) -> 0
  *params = (pname == 0x8B84) ? 0 : 1;
}

void glGetProgramivShim(uint32_t program, uint32_t pname, int32_t* params) {
  if (!params) return;
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t, uint32_t, int32_t*);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glGetProgramiv"));
    if (realFn) {
      realFn(program, pname, params);
      return;
    }
  }
  // GL_LINK_STATUS (0x8B82) / GL_VALIDATE_STATUS (0x8B83) -> GL_TRUE (1); GL_INFO_LOG_LENGTH -> 0
  *params = (pname == 0x8B84) ? 0 : 1;
}

void glGetIntegervShim(uint32_t pname, int32_t* params) {
  if (!params) return;
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t, int32_t*);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glGetIntegerv"));
    if (realFn) {
      realFn(pname, params);
      return;
    }
  }
  switch (pname) {
    case 0x0D33: *params = 4096; break;  // GL_MAX_TEXTURE_SIZE
    case 0x8869: *params = 16; break;    // GL_MAX_VERTEX_ATTRIBS
    case 0x8DFB: *params = 256; break;   // GL_MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: *params = 256; break;   // GL_MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8B4D: *params = 16; break;    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
    case 0x8872: *params = 16; break;    // GL_MAX_TEXTURE_IMAGE_UNITS
    case 0x84E8: *params = 4096; break;  // GL_MAX_RENDERBUFFER_SIZE
    case 0x0BA2:                         // GL_VIEWPORT
      params[0] = 0; params[1] = 0; params[2] = g_surfaceWidth; params[3] = g_surfaceHeight;
      break;
    default: *params = 0; break;
  }
}

void glClearColorShim(float r, float g, float b, float a) {
  noteCompatCall("glClearColor");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(float, float, float, float);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glClearColor"));
    if (realFn) realFn(r, g, b, a);
  }
}

void glClearShim(uint32_t mask) {
  noteCompatCall("glClear");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glClear"));
    if (realFn) realFn(mask);
  }
}

void glViewportShim(int x, int y, int w, int h) {
  noteCompatCall("glViewport");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(int, int, int, int);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glViewport"));
    if (realFn) realFn(x, y, w, h);
  }
}

void glDrawArraysShim(uint32_t mode, int first, int count) {
  noteCompatCall("glDrawArrays");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t, int, int);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glDrawArrays"));
    if (realFn) realFn(mode, first, count);
  }
}

void glDrawElementsShim(uint32_t mode, int count, uint32_t type, const void* indices) {
  noteCompatCall("glDrawElements");
  NativeGles& ng = nativeGles();
  if (ng.hasCurrentHardwareContext()) {
    using Fn = void (*)(uint32_t, int, uint32_t, const void*);
    static auto realFn = reinterpret_cast<Fn>(ng.resolveGl("glDrawElements"));
    if (realFn) realFn(mode, count, type, indices);
  }
}

using Fn = uint64_t;
void addShim(relinker::CompatRegistry& reg, const char* name, Fn fn, const char* fw) {
  reg.add(name, fn, {compat::SymbolClass::CompatibilityShim, fw, "shim"});
  ++g_count;
}

void addGlesEntry(relinker::CompatRegistry& reg, const char* machoName, uint64_t fallbackFn) {
  const char* elfName = (machoName[0] == '_') ? (machoName + 1) : machoName;
  void* resolved = nativeGles().resolveGl(elfName);
  // Use the explicit shim if provided, or the direct libGLESv2 symbol if resolved on device,
  // or the fallback no-op shim.
  uint64_t target = fallbackFn ? fallbackFn : (resolved ? reinterpret_cast<uint64_t>(resolved) : reinterpret_cast<Fn>(voidShim));
  reg.add(machoName, target, {compat::SymbolClass::AndroidBackend, "OpenGLES", "gles2_forward"});
  ++g_count;
  ++g_glesCount;
}

// DummyFrameworkClass instances keep stable addresses for the process lifetime.
DummyFrameworkClass& classStub(const char* name) {
  static auto& all = *new std::vector<DummyFrameworkClass*>();
  for (auto* c : all)
    if (std::strcmp(c->name, name) == 0) return *c;
  auto* c = new DummyFrameworkClass();
  std::snprintf(c->name, sizeof c->name, "%s", name);
  all.push_back(c);
  return *c;
}

void addClassPair(relinker::CompatRegistry& reg, const char* cls, const char* fw) {
  auto& stub = classStub(cls);
  uint64_t addr = reinterpret_cast<uint64_t>(&stub);
  reg.add(std::string("_OBJC_CLASS_$_") + cls, addr, {compat::SymbolClass::NoopStub, fw, "dummy_class"});
  reg.add(std::string("_OBJC_METACLASS_$_") + cls, addr, {compat::SymbolClass::NoopStub, fw, "dummy_class"});
  g_count += 2;
}

void addData(relinker::CompatRegistry& reg, const char* name, const char* fw, const char* label) {
  static auto& blocks = *new std::vector<DummyFrameworkClass*>();  // see classStub()
  auto* b = new DummyFrameworkClass();
  std::snprintf(b->name, sizeof b->name, "%s", label);
  blocks.push_back(b);
  reg.add(name, reinterpret_cast<uint64_t>(b), {compat::SymbolClass::NoopStub, fw, "dummy_data"});
  ++g_count;
}

}  // namespace

void registerFrameworkStubs(relinker::CompatRegistry& reg) {
  using namespace compat;

  // --- libobjc runtime family (selector-aware objc_msgSend + retain/release family) --------
  addShim(reg, "_objc_msgSend", reinterpret_cast<Fn>(objcMsgSendShim), "libobjc");
  for (const char* n : {"_objc_msgSendSuper", "_objc_msgSendSuper2",
                        "_objc_msgSend_stret", "_objc_retain", "_objc_retainBlock",
                        "_objc_retainAutorelease", "_objc_autorelease",
                        "_objc_autoreleaseReturnValue", "_objc_retainAutoreleasedReturnValue",
                        "_objc_unsafeClaimAutoreleasedReturnValue", "_objc_loadWeakRetained",
                        "_objc_rootRetain", "_CFRetain"})
    addShim(reg, n, reinterpret_cast<Fn>(passthrough), "libobjc");
  for (const char* n : {"_objc_release", "_objc_storeStrong", "_objc_storeWeak",
                        "_objc_destroyWeak", "_objc_clear_deallocating", "_objc_setProperty",
                        "_objc_setProperty_nonatomic", "_objc_copyWeak", "_objc_moveWeak",
                        "_objc_autoreleasePoolPop"})
    addShim(reg, n, reinterpret_cast<Fn>(voidShim1), "libobjc");
  addShim(reg, "_objc_autoreleasePoolPush", reinterpret_cast<Fn>(poolPushShim), "libobjc");
  for (const char* n : {"_sel_registerName", "_sel_getUid"})
    addShim(reg, n, reinterpret_cast<Fn>(passthrough), "libobjc");
  addShim(reg, "_sel_getName", reinterpret_cast<Fn>(emptyStringShim), "libobjc");
  for (const char* n : {"_objc_getClass", "_objc_lookUpClass", "_objc_getMetaClass",
                        "_objc_getRequiredClass", "_objc_getAssociatedObject",
                        "_class_isMetaClass", "_class_respondsToSelector", "_object_getClass"})
    addShim(reg, n, reinterpret_cast<Fn>(zeroShim), "libobjc");
  addShim(reg, "_objc_setAssociatedObject", reinterpret_cast<Fn>(voidShim2), "libobjc");
  addShim(reg, "_objc_exception_throw", reinterpret_cast<Fn>(objcExceptionThrowShim), "libobjc");

  // --- CoreFoundation ----------------------------------------------------------------------
  addShim(reg, "_CFRelease", reinterpret_cast<Fn>(voidShim1), "CoreFoundation");
  for (const char* n : {"_CFGetTypeID", "_CFStringGetLength", "_CFStringCreateWithCString",
                        "_CFStringCreateMutable", "_CFBundleGetMainBundle", "_CFRunLoopGetCurrent",
                        "_CFRunLoopRunInMode", "_CFPreferencesCopyAppValue", "_CFShow"})
    addShim(reg, n, reinterpret_cast<Fn>(zeroShim), "CoreFoundation");
  addShim(reg, "_CFRunLoopRun", reinterpret_cast<Fn>(voidShim), "CoreFoundation");
  addShim(reg, "_CFRunLoopStop", reinterpret_cast<Fn>(voidShim1), "CoreFoundation");
  addShim(reg, "_CFAbsoluteTimeGetCurrent", reinterpret_cast<Fn>(zeroDoubleShim), "CoreFoundation");

  // --- UIKit --------------------------------------------------------------------------------
  addShim(reg, "_UIApplicationMain", reinterpret_cast<Fn>(uiApplicationMainShim), "UIKit");
  for (const char* n : {"_UIGraphicsGetCurrentContext", "_UIGraphicsGetImageFromCurrentImageContext",
                        "_UIImagePNGRepresentation", "_UIImageJPEGRepresentation"})
    addShim(reg, n, reinterpret_cast<Fn>(zeroShim), "UIKit");
  for (const char* n : {"_UIGraphicsBeginImageContext", "_UIGraphicsEndImageContext",
                        "_UIImageWriteToSavedPhotosAlbum"})
    addShim(reg, n, reinterpret_cast<Fn>(voidShim), "UIKit");
  addShim(reg, "_UIGraphicsBeginImageContextWithOptions", reinterpret_cast<Fn>(voidShim), "UIKit");

  // --- Foundation (with sandboxed NSHomeDirectory / NSSearchPathForDirectoriesInDomains) ---
  addShim(reg, "_NSLog", reinterpret_cast<Fn>(nsLogShim), "Foundation");
  addShim(reg, "_NSHomeDirectory", reinterpret_cast<Fn>(guestNSHomeDirectory), "Foundation");
  addShim(reg, "_NSTemporaryDirectory", reinterpret_cast<Fn>(guestNSTemporaryDirectory), "Foundation");
  addShim(reg, "_NSSearchPathForDirectoriesInDomains",
          reinterpret_cast<Fn>(guestNSSearchPathForDirectoriesInDomains), "Foundation");
  for (const char* n : {"_NSUserName", "_NSFullUserName",
                        "_NSGetUncaughtExceptionHandler", "_NSClassFromString"})
    addShim(reg, n, reinterpret_cast<Fn>(zeroShim), "Foundation");
  addShim(reg, "_NSStringFromClass", reinterpret_cast<Fn>(emptyStringShim), "Foundation");
  addShim(reg, "_NSSetUncaughtExceptionHandler", reinterpret_cast<Fn>(voidShim1), "Foundation");

  // --- QuartzCore ---------------------------------------------------------------------------
  addShim(reg, "_CACurrentMediaTime", reinterpret_cast<Fn>(zeroDoubleShim), "QuartzCore");

  // --- EAGL C-level entrypoints & helpers ---------------------------------------------------
  addShim(reg, "_EAGLGetCurrentContext", reinterpret_cast<Fn>(eaglGetCurrentContext), "OpenGLES");
  addShim(reg, "_EAGLSetCurrentContext", reinterpret_cast<Fn>(eaglSetCurrentContext), "OpenGLES");

  // --- OpenGL ES 2.0 -> Android EGL/GLESv2 forwarding table --------------------------------
  addGlesEntry(reg, "_glGetString", reinterpret_cast<Fn>(glGetStringShim));
  addGlesEntry(reg, "_glGetError", reinterpret_cast<Fn>(glGetErrorShim));
  addGlesEntry(reg, "_glCheckFramebufferStatus", reinterpret_cast<Fn>(glCheckFramebufferStatusShim));
  addGlesEntry(reg, "_glGenBuffers", reinterpret_cast<Fn>(glGenBuffersShim));
  addGlesEntry(reg, "_glGenTextures", reinterpret_cast<Fn>(glGenTexturesShim));
  addGlesEntry(reg, "_glGenFramebuffers", reinterpret_cast<Fn>(glGenFramebuffersShim));
  addGlesEntry(reg, "_glGenRenderbuffers", reinterpret_cast<Fn>(glGenRenderbuffersShim));
  addGlesEntry(reg, "_glGenVertexArraysOES", reinterpret_cast<Fn>(glGenVertexArraysOESShim));
  addGlesEntry(reg, "_glCreateShader", reinterpret_cast<Fn>(glCreateShaderShim));
  addGlesEntry(reg, "_glCreateProgram", reinterpret_cast<Fn>(glCreateProgramShim));
  addGlesEntry(reg, "_glGetShaderiv", reinterpret_cast<Fn>(glGetShaderivShim));
  addGlesEntry(reg, "_glGetProgramiv", reinterpret_cast<Fn>(glGetProgramivShim));
  addGlesEntry(reg, "_glGetIntegerv", reinterpret_cast<Fn>(glGetIntegervShim));
  addGlesEntry(reg, "_glClearColor", reinterpret_cast<Fn>(glClearColorShim));
  addGlesEntry(reg, "_glClear", reinterpret_cast<Fn>(glClearShim));
  addGlesEntry(reg, "_glViewport", reinterpret_cast<Fn>(glViewportShim));
  addGlesEntry(reg, "_glDrawArrays", reinterpret_cast<Fn>(glDrawArraysShim));
  addGlesEntry(reg, "_glDrawElements", reinterpret_cast<Fn>(glDrawElementsShim));

  // Remaining OpenGL ES 2.0 standard entry points forwarded directly to Android's libGLESv2.so
  const char* gles2Functions[] = {
      "_glActiveTexture", "_glAttachShader", "_glBindAttribLocation", "_glBindBuffer",
      "_glBindFramebuffer", "_glBindRenderbuffer", "_glBindTexture", "_glBindVertexArrayOES",
      "_glBlendColor", "_glBlendEquation", "_glBlendEquationSeparate", "_glBlendFunc",
      "_glBlendFuncSeparate", "_glBufferData", "_glBufferSubData", "_glClearDepthf",
      "_glClearStencil", "_glColorMask", "_glCompileShader", "_glCompressedTexImage2D",
      "_glCompressedTexSubImage2D", "_glCopyTexImage2D", "_glCopyTexSubImage2D", "_glCullFace",
      "_glDeleteBuffers", "_glDeleteFramebuffers", "_glDeleteProgram", "_glDeleteRenderbuffers",
      "_glDeleteShader", "_glDeleteTextures", "_glDeleteVertexArraysOES", "_glDepthFunc",
      "_glDepthMask", "_glDepthRangef", "_glDetachShader", "_glDisable",
      "_glDisableVertexAttribArray", "_glDiscardFramebufferEXT", "_glEnable",
      "_glEnableVertexAttribArray", "_glFinish", "_glFlush", "_glFramebufferRenderbuffer",
      "_glFramebufferTexture2D", "_glFrontFace", "_glGenerateMipmap", "_glGetActiveAttrib",
      "_glGetActiveUniform", "_glGetAttachedShaders", "_glGetAttribLocation", "_glGetBooleanv",
      "_glGetBufferParameteriv", "_glGetFloatv", "_glGetFramebufferAttachmentParameteriv",
      "_glGetProgramInfoLog", "_glGetRenderbufferParameteriv", "_glGetShaderInfoLog",
      "_glGetShaderPrecisionFormat", "_glGetShaderSource", "_glGetTexParameterfv",
      "_glGetTexParameteriv", "_glGetUniformfv", "_glGetUniformiv", "_glGetUniformLocation",
      "_glGetVertexAttribfv", "_glGetVertexAttribiv", "_glGetVertexAttribPointerv", "_glHint",
      "_glIsBuffer", "_glIsEnabled", "_glIsFramebuffer", "_glIsProgram", "_glIsRenderbuffer",
      "_glIsShader", "_glIsTexture", "_glIsVertexArrayOES", "_glLineWidth", "_glLinkProgram",
      "_glMapBufferOES", "_glPixelStorei", "_glPolygonOffset", "_glReadPixels",
      "_glReleaseShaderCompiler", "_glRenderbufferStorage",
      "_glRenderbufferStorageMultisampleAPPLE", "_glResolveMultisampleFramebufferAPPLE",
      "_glSampleCoverage", "_glScissor", "_glShaderBinary", "_glShaderSource", "_glStencilFunc",
      "_glStencilFuncSeparate", "_glStencilMask", "_glStencilMaskSeparate", "_glStencilOp",
      "_glStencilOpSeparate", "_glTexImage2D", "_glTexParameterf", "_glTexParameterfv",
      "_glTexParameteri", "_glTexParameteriv", "_glTexSubImage2D", "_glUniform1f",
      "_glUniform1fv", "_glUniform1i", "_glUniform1iv", "_glUniform2f", "_glUniform2fv",
      "_glUniform2i", "_glUniform2iv", "_glUniform3f", "_glUniform3fv", "_glUniform3i",
      "_glUniform3iv", "_glUniform4f", "_glUniform4fv", "_glUniform4i", "_glUniform4iv",
      "_glUniformMatrix2fv", "_glUniformMatrix3fv", "_glUniformMatrix4fv", "_glUnmapBufferOES",
      "_glUseProgram", "_glValidateProgram", "_glVertexAttrib1f", "_glVertexAttrib1fv",
      "_glVertexAttrib2f", "_glVertexAttrib2fv", "_glVertexAttrib3f", "_glVertexAttrib3fv",
      "_glVertexAttrib4f", "_glVertexAttrib4fv", "_glVertexAttribPointer",
  };
  for (const char* glFn : gles2Functions) addGlesEntry(reg, glFn, 0);

  // --- dummy classes: the essential UIKit / EAGL / QuartzCore / Foundation set --------------
  const char* foundationClasses[] = {
      "NSObject", "NSProxy", "NSAutoreleasePool", "NSBundle", "NSString", "NSMutableString",
      "NSArray", "NSMutableArray", "NSDictionary", "NSMutableDictionary", "NSSet", "NSMutableSet",
      "NSNumber", "NSData", "NSMutableData", "NSDate", "NSError", "NSException", "NSValue",
      "NSNull", "NSURL", "NSThread", "NSTimer", "NSNotification", "NSNotificationCenter",
      "NSOperationQueue", "NSRunLoop", "NSUserDefaults", "NSFileManager", "NSLocale",
      "NSIndexPath", "NSProcessInfo", "NSUUID", "NSCache",
  };
  for (const char* c : foundationClasses) addClassPair(reg, c, "Foundation");
  const char* uikitClasses[] = {
      "UIResponder", "UIApplication", "UIScreen", "UIView", "UIViewController",
      "UINavigationController", "UITabBarController", "UIImagePickerController", "UIImage",
      "UIImageView", "UILabel", "UIButton", "UIControl", "UIColor", "UIFont", "UIEvent",
      "UITouch", "UIGestureRecognizer", "UITapGestureRecognizer", "UIPanGestureRecognizer",
      "UIScrollView", "UITableView", "UITableViewCell", "UITextField", "UITextView",
      "UIActivityIndicatorView", "UIAlertView", "UIActionSheet", "UIDevice", "UIBezierPath",
      "UIWebView",
  };
  for (const char* c : uikitClasses) addClassPair(reg, c, "UIKit");
  const char* quartzClasses[] = {"CALayer", "CAEAGLLayer", "CAAnimation", "CABasicAnimation"};
  for (const char* c : quartzClasses) addClassPair(reg, c, "QuartzCore");

  // EAGL + UIWindow + CADisplayLink + UIApplication get specialized dummy wrappers.
  reg.add("_OBJC_CLASS_$_EAGLContext", reinterpret_cast<uint64_t>(&g_dummyEAGLContext.classStub),
          {SymbolClass::NoopStub, "OpenGLES", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_EAGLContext", reinterpret_cast<uint64_t>(&g_dummyEAGLContext.classStub),
          {SymbolClass::NoopStub, "OpenGLES", "dummy_class"});
  reg.add("_OBJC_CLASS_$_EAGLSharegroup", reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub),
          {SymbolClass::NoopStub, "OpenGLES", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_EAGLSharegroup", reinterpret_cast<uint64_t>(&g_dummyEAGLSharegroup.classStub),
          {SymbolClass::NoopStub, "OpenGLES", "dummy_class"});
  reg.add("_OBJC_CLASS_$_UIWindow", reinterpret_cast<uint64_t>(&g_dummyUIWindow.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_UIWindow", reinterpret_cast<uint64_t>(&g_dummyUIWindow.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_CLASS_$_CADisplayLink", reinterpret_cast<uint64_t>(&g_dummyCADisplayLink.classStub),
          {SymbolClass::NoopStub, "QuartzCore", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_CADisplayLink", reinterpret_cast<uint64_t>(&g_dummyCADisplayLink.classStub),
          {SymbolClass::NoopStub, "QuartzCore", "dummy_class"});
  std::snprintf(g_dummyEAGLContext.classStub.name, sizeof g_dummyEAGLContext.classStub.name, "EAGLContext");
  std::snprintf(g_dummyEAGLSharegroup.classStub.name, sizeof g_dummyEAGLSharegroup.classStub.name, "EAGLSharegroup");
  std::snprintf(g_dummyUIApplication.classStub.name, sizeof g_dummyUIApplication.classStub.name, "UIApplication");
  std::snprintf(g_dummyUIWindow.classStub.name, sizeof g_dummyUIWindow.classStub.name, "UIWindow");
  std::snprintf(g_dummyCADisplayLink.classStub.name, sizeof g_dummyCADisplayLink.classStub.name, "CADisplayLink");
  std::snprintf(g_dummyUITouch.classStub.name, sizeof g_dummyUITouch.classStub.name, "UITouch");
  std::snprintf(g_dummyUIEvent.classStub.name, sizeof g_dummyUIEvent.classStub.name, "UIEvent");
  std::snprintf(g_dummyUIScreen.classStub.name, sizeof g_dummyUIScreen.classStub.name, "UIScreen");
  g_count += 14;  // 6 earlier pair-registrations + 2 new (UITouch, UIEvent, UIScreen pairs = 6 more)

  reg.add("_OBJC_CLASS_$_UITouch", reinterpret_cast<uint64_t>(&g_dummyUITouch.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_UITouch", reinterpret_cast<uint64_t>(&g_dummyUITouch.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_CLASS_$_UIEvent", reinterpret_cast<uint64_t>(&g_dummyUIEvent.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_UIEvent", reinterpret_cast<uint64_t>(&g_dummyUIEvent.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_CLASS_$_UIScreen", reinterpret_cast<uint64_t>(&g_dummyUIScreen.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});
  reg.add("_OBJC_METACLASS_$_UIScreen", reinterpret_cast<uint64_t>(&g_dummyUIScreen.classStub),
          {SymbolClass::NoopStub, "UIKit", "dummy_class"});

  const char* avClasses[] = {"AVAudioPlayer", "AVAudioSession", "AVPlayer"};
  for (const char* c : avClasses) addClassPair(reg, c, "AVFoundation");
  const char* skClasses[] = {"SKPaymentQueue", "SKPayment", "SKPaymentTransaction",
                             "SKProductsRequest", "SKProduct"};
  for (const char* c : skClasses) addClassPair(reg, c, "StoreKit");
  addClassPair(reg, "ASIdentifierManager", "AdSupport");

  // --- data constants the startup & EAGL code references ------------------------------------
  for (const char* n : {"_NSConcreteStackBlock", "_NSConcreteGlobalBlock", "_NSConcreteMallocBlock"})
    addData(reg, n, "libSystem", "NSConcreteBlock");
  for (const char* n : {"_kCFAllocatorDefault", "_kCFAllocatorMalloc", "_kCFAllocatorNull",
                        "_kCFAllocatorSystemDefault", "_kCFRunLoopDefaultMode",
                        "_kCFRunLoopCommonModes"})
    addData(reg, n, "CoreFoundation", n + 4);
  for (const char* n : {"_kEAGLDrawablePropertyRetainedBacking", "_kEAGLDrawablePropertyColorFormat",
                        "_kEAGLColorFormatRGBA8", "_kEAGLColorFormatRGB565"})
    addData(reg, n, "OpenGLES", n + 1);
  addData(reg, "_NSFoundationVersionNumber", "Foundation", "NSFoundationVersionNumber");
  for (const char* n : {"_UIWindowLevelNormal", "_UIWindowLevelAlert"})
    addData(reg, n, "UIKit", n + 1);
  for (const char* n : {"_UIApplicationDidFinishLaunchingNotification",
                        "_UIApplicationWillResignActiveNotification",
                        "_UIApplicationDidEnterBackgroundNotification",
                        "_UIApplicationWillEnterForegroundNotification",
                        "_UIApplicationDidBecomeActiveNotification",
                        "_UIKeyboardWillShowNotification", "_UIKeyboardDidShowNotification",
                        "_UIKeyboardWillHideNotification", "_UIKeyboardDidHideNotification"})
    addData(reg, n, "UIKit", n + 1);
}

size_t frameworkStubCount() { return g_count; }
size_t glesBridgeSymbolCount() { return g_glesCount; }

}  // namespace radeki::runtime
