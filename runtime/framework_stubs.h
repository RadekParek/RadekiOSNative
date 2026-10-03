// Minimal framework stub layer + EAGL/UIKit/OpenGL ES 2.0 bridge.
//
// Four kinds of entries, all registered into a CompatRegistry by registerFrameworkStubs():
//
//  1. Written shims for symbols whose *call itself* matters for the startup and main loop:
//     NSLog, UIApplicationMain, the objc memory-management family, selector-aware objc_msgSend
//     (bridging EAGLContext, UIWindow, CADisplayLink and Foundation sandbox paths),
//     sel_registerName, autorelease pools, Foundation path functions (NSHomeDirectory,
//     NSSearchPathForDirectoriesInDomains, NSTemporaryDirectory), and CoreFoundation basics.
//
//  2. EAGLContext / UIWindow / CADisplayLink main-loop state and OpenGL ES 2.0 forwarding:
//     setCurrentContext, presentRenderbuffer, renderbufferStorage:fromDrawable:,
//     makeKeyAndVisible, displayLinkWithTarget:selector:, addToRunLoop:forMode:, and direct
//     forwarding of OpenGL ES 2.0 entry points (_gl*) onto Android's native libEGL / libGLESv2.
//
//  3. Dummy C++ structures backing _OBJC_CLASS_$_ / _OBJC_METACLASS_$_ data imports for the
//     essential UIKit, EAGL, QuartzCore and Foundation classes.
//
//  4. Data constants (_NSConcreteStackBlock, kCFAllocatorDefault, NSFoundationVersionNumber,
//     kEAGLDrawableProperty*, notification names) as labelled zeroed blocks.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "relinker/relinker.h"

namespace radeki::runtime {

// objc-class-shaped dummy backing store. All real fields zero; the label is diagnostic.
struct DummyFrameworkClass {
  uint64_t isa = 0;
  uint64_t superclass = 0;
  uint64_t cacheBuckets = 0;
  uint64_t cacheMaskVtable = 0;
  uint64_t dataBits = 0;  // class_data_bits_t: zero -> any real dereference faults visibly
  char name[24] = {};
};
static_assert(sizeof(DummyFrameworkClass) == 64, "dummy class stays one cache line");

// Minimal wrappers for EAGL / UIWindow / CADisplayLink / UIApplication startup & loop essentials.
struct DummyEAGLSharegroup {
  DummyFrameworkClass classStub;
  uint32_t refcount = 1;
};
struct DummyEAGLContext {
  DummyFrameworkClass classStub;
  uint32_t api = 2;               // kEAGLRenderingAPIOpenGLES2 = 2 by default
  uint64_t sharegroup = 0;
  uint32_t multiThreaded = 0;
  uint32_t isCurrent = 0;
  uint64_t eglDisplay = 0;
  uint64_t eglSurface = 0;
  uint64_t eglContext = 0;
  uint64_t presentedFrames = 0;
};
struct DummyUIApplication {
  DummyFrameworkClass classStub;
  uint64_t delegate = 0;
  uint32_t applicationState = 0;  // UIApplicationStateActive
  uint32_t idleTimerDisabled = 0;
};
struct DummyUIWindow {
  DummyFrameworkClass classStub;
  uint64_t rootViewController = 0;
  uint32_t keyAndVisible = 0;
  uint32_t width = 1280;
  uint32_t height = 720;
  double scale = 2.0;
};
struct DummyCADisplayLink {
  DummyFrameworkClass classStub;
  uint64_t target = 0;
  uint64_t selector = 0;
  uint64_t runLoop = 0;
  uint64_t mode = 0;
  uint32_t frameInterval = 1;
  uint32_t paused = 0;
  uint32_t registeredInRunLoop = 0;
  double timestamp = 0.0;
  double duration = 1.0 / 60.0;
};

// Minimal UIKit touch/event objects so the guest can call -[UIEvent allTouches], -[NSSet anyObject],
// -[UITouch locationInView:], -[UITouch phase] and receive coordinates/phase reflecting the most
// recent Android MotionEvent forwarded via injectTouchEvent.
struct DummyUITouch {
  DummyFrameworkClass classStub;
  float x = 0, y = 0;   // pixels (matches window coordinates; divide by scale for points)
  int phase = 0;        // UITouchPhaseBegan=0, Moved=1, Stationary=2, Ended=3, Cancelled=4
  double timestamp = 0;
};
struct DummyUIEvent {
  DummyFrameworkClass classStub;
  DummyUITouch touch;
};
struct DummyUIScreen {
  DummyFrameworkClass classStub;
  double scale = 2.0;
  uint32_t boundsW = 640;
  uint32_t boundsH = 360;
};

// Direct C entry points for the EAGL / UIWindow / CADisplayLink bridge.
DummyEAGLContext* eaglDefaultContext();
DummyEAGLContext* eaglInitWithAPI(void* self, uint32_t api);
int eaglSetCurrentContext(void* ctx);
DummyEAGLContext* eaglGetCurrentContext();
int eaglPresentRenderbuffer(void* self, uint32_t target);
int eaglRenderbufferStorageFromDrawable(void* self, uint32_t target, void* drawable);
void bindAndroidEglWindow(void* nativeWindow, int width, int height);

// Recreate the EGL window surface (e.g. on surface change). If nativeWindow is null, detaches.
void updateAndroidEglWindow(void* nativeWindow, int width, int height);

// Signals the UIApplicationMain event loop to exit (app teardown / surface destroyed).
void requestExitUiLoop(int exitCode);

// Inject a touch event from Android MotionEvent. action: 0=DOWN, 1=UP, 2=MOVE.
// Coordinates are in pixels (will be scaled to guest screen points via UIWindow scale).
void injectTouchEvent(int action, float x, float y, int64_t timestampMs);

// Query the current EGL/drawable size (for glViewport / renderbufferStorage).
void eglDrawableSize(int* outWidth, int* outHeight);

// Snapshot used by the Android black-screen overlay and included in the durable run report.
struct GraphicsStatus {
  bool eventLoopActive = false;
  bool nativeWindowBound = false;
  bool eglReady = false;
  bool windowSurface = false;
  bool contextCurrent = false;
  bool displayLinkRegistered = false;
  int width = 0;
  int height = 0;
  uint64_t uiFrames = 0;
  uint64_t presentedFrames = 0;
  std::string lastEglIssue;
};
GraphicsStatus currentGraphicsStatus();

DummyUIWindow* uiWindowDefault();
int uiWindowMakeKeyAndVisible(void* window);

DummyCADisplayLink* caDisplayLinkDefault();
DummyCADisplayLink* caDisplayLinkWithTargetSelector(uint64_t target, uint64_t selector);
int caDisplayLinkAddToRunLoop(void* link, uint64_t runLoop, uint64_t mode);

// Registers every hand-written framework stub and GLESv2 bridge entry into `reg`.
// Called by makeHostRuntime(); deliberately NOT part of makeCompatRegistry().
void registerFrameworkStubs(relinker::CompatRegistry& reg);
size_t frameworkStubCount();
size_t glesBridgeSymbolCount();

}  // namespace radeki::runtime
