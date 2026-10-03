// Runtime: maps a relinked ARM64 or ARMv7 image into the matching native Android process and
// executes the ORIGINAL AArch64/AArch32 code. ARM32 guests require the separate armeabi-v7a APK;
// AArch32 and AArch64 instructions are never mixed inside one process.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "loader/dyld.h"
#include "mach_o/macho.h"
#include "relinker/relinker.h"

namespace radeki::runtime {

struct RunResult {
  bool ran = false;          // guest main returned or called exit/abort
  bool crashed = false;      // guest raised a fatal signal
  int exitCode = 0;
  std::string signalName, trapSymbol, error, output;
  uint64_t faultPc = 0, faultAddr = 0;
  std::string trapClass, subsystem, resolutionMethod, callSymbol, image;
  uint64_t callAddress = 0;
  int64_t threadId = 0;
  std::vector<std::string> recentCalls;
  size_t dispatchStubs = 0;  // imports bound to logging no-op stubs instead of traps
};

// --- POSIX External Storage Sandboxing (/storage/emulated/0/RadekiOSNative/sandbox/) -------

inline constexpr const char* kDefaultSandboxBase = "/storage/emulated/0/RadekiOSNative/sandbox/";
// Android scoped-storage fallback when the primary /storage/emulated/0/RadekiOSNative/sandbox/
// cannot be created (e.g. on Android 11+ without MANAGE_EXTERNAL_STORAGE).
inline constexpr const char* kScopedFallbackSandboxBase =
    "/storage/emulated/0/Android/data/org.radekiosnative.recompiler/files/sandbox/";

// Returns the active sandbox base path (always trailing-slash normalized).
std::string sandboxBasePath();
// Overrides the sandbox base path (pass "" to restore kDefaultSandboxBase).
void setSandboxBasePath(const std::string& path);
// Returns the 4 standard iOS container directories under the active sandbox base:
//   - /storage/emulated/0/RadekiOSNative/sandbox/Documents/
//   - /storage/emulated/0/RadekiOSNative/sandbox/Library/Application Support/
//   - /storage/emulated/0/RadekiOSNative/sandbox/Library/Caches/
//   - /storage/emulated/0/RadekiOSNative/sandbox/tmp/
std::vector<std::string> standardSandboxDirectories();
// Automatically constructs the standard iOS container directories if they do not exist.
bool ensureSandboxDirectories();
// Verified sandbox state, checked during launch initialization and surfaced in the Android
// diagnostics and the durable run report: the tree (including Documents/games/com.mojang/)
// must exist at the primary base or one of the external fallbacks.
struct SandboxStatus {
  bool ready = false;           // the full standard tree exists at the active host base
  std::string base;             // logical sandbox base the guest sees (NSHomeDirectory)
  std::string hostBase;         // actual host location backing it (differs when falling back)
  bool usingFallback = false;   // primary /storage/emulated/0 base was not writable
  std::string gamesMojangPath;  // host path of Documents/games/com.mojang (MCPE data folder)
};
SandboxStatus currentSandboxStatus();
// Rewrites a guest iOS filesystem path into the sandbox folder.
std::string translateGuestPath(const std::string& guestPath);
// Maps a logical sandbox path onto the writable host filesystem path (identical on Android
// where /storage/emulated/0 is present; falls back to a host-writable mirror only if the
// kernel denies creating /storage on a non-Android build host).
std::string resolveSandboxHostPath(const std::string& guestOrSandboxPath);

// Foundation path functions (also registered as _NSHomeDirectory, _NSTemporaryDirectory,
// _NSSearchPathForDirectoriesInDomains in both the compat registry and framework stubs).
const char* guestNSHomeDirectory();
const char* guestNSTemporaryDirectory();
const void* guestNSSearchPathForDirectoriesInDomains(uint64_t directory, uint64_t domainMask, int expandTilde);

// Compatibility "libSystem" subset (non-variadic, ABI-identical functions + sandboxed POSIX
// file interceptors), the written libc++abi shims, and the libc++.1.dylib -> host C++
// forwarding table. Framework dummy classes/stubs are NOT included here (see makeHostRuntime
// / registerFrameworkStubs).
relinker::CompatRegistry makeCompatRegistry();

// The complete host runtime layer handed to the loader as `Options::fallback`:
//   registry      = makeCompatRegistry() + framework shims + dummy UIKit/EAGL/Foundation objects
//   dlsym         = chained behind the registry: dlsym pass-through onto the host libc++
//   stubFactory   = logging no-op dispatch stubs for whatever remains unresolved
// Owns everything; keep it alive for the whole link+run cycle.
class HostCxxResolver;  // runtime/cxx_forward.h: dlsym pass-through onto the host libc++
struct HostRuntime {
  relinker::CompatRegistry registry;
  HostCxxResolver* dlsym = nullptr;  // owned; chained as registry.chainNext()
  relinker::StubFactory stubFactory;
  ~HostRuntime();
  HostRuntime() = default;
  HostRuntime(HostRuntime&&) noexcept;
  HostRuntime& operator=(HostRuntime&&) noexcept;
};
HostRuntime makeHostRuntime();

std::string describeResult(const RunResult&);

// The Android launcher opens one private per-run file before binding its Surface. Guest output,
// EGL/runtime events and optional RTLS call samples are appended as they happen. Logging is
// inert when no run-log file is active; RTLS adds sampled detail and heartbeats when enabled.
void beginRunLog(const std::string& path, bool realtimeLogging);
void endRunLog();
void writeRunLog(const std::string& text);
void logRunEvent(const std::string& event);
bool realtimeLoggingEnabled();

// Keep literal-name tracing cheap in hot compatibility wrappers: the const-char overload
// avoids constructing a temporary std::string for each guest call.
void noteCompatCall(const char*);
void noteCompatCall(const std::string&);
std::vector<std::string> recentCompatCalls();
void resetRecentCompatCalls();
std::string takeGuestOutput();
void appendGuestOutput(const std::string& s);
[[noreturn]] void abortGuest(int code, const char* reason);  // unwinds out of guest code

RunResult run(const relinker::LinkedImage& li);
// Link (trying several load bases) and run an image with the compat registry.
// useStubs=true additionally enables the host runtime layer: framework shims/dummies and the
// logging dispatch-stub factory, so unbound imports keep the startup loop alive instead of
// trapping (the run still reports them via RunResult::dispatchStubs).
RunResult runImage(const macho::Image& img, bool useStubs = false);

// One entry per image handed to runLoadedImage(), in link order.
struct ImageRun {
  std::string path;
  bool ran = false;       // this image's entry point returned (or the process exited inside it)
  bool crashed = false;
  bool skipped = false;   // never started: an earlier image exited the process
  int exitCode = 0;
  std::string signalName, trapSymbol, error;
  uint64_t faultPc = 0, faultAddr = 0;
};

struct MultiRunResult {
  bool ran = false;        // the run reached the end of the image list without crashing
  bool crashed = false;
  int exitCode = 0;        // exit status of the image that ended the run
  std::string error, output;
  std::vector<ImageRun> images;
  size_t dispatchStubs = 0;          // imports bound to logging no-op stubs instead of traps
  std::vector<std::string> stubbed;  // their names (capped), for reports
  bool ok() const { return error.empty() && !crashed; }
};

// Links every image in the registry (using `opt.fallback` as the last-resort resolver, i.e.
// the compat libSystem) and runs them inside ONE process: a single reservation covers every
// image, so their slides are what dyld would give them, and cross-image pointers resolve.
// Images are started in the registry's link order (dependencies before importers).
// Like run(), this requires the process ABI to match every linked guest image; mismatches are reported.
MultiRunResult runLoadedImage(loader::Registry& reg, const loader::Options& opt);

}  // namespace radeki::runtime
