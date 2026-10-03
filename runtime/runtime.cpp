#include "runtime/runtime.h"
#include "runtime/compat_cxx.h"
#include "runtime/cxx_forward.h"
#include "runtime/framework_stubs.h"
#include "runtime/stub_dispatch.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace radeki::runtime {
namespace {

std::mutex g_runMutex;
std::mutex g_runLogMutex;
std::mutex g_rtlsCallsMutex;
std::unordered_set<std::string> g_rtlsCallNames;
std::atomic<bool> g_rtlsEnabled{false};
int g_runLogFd = -1;
uint64_t g_runLogBytes = 0;
bool g_runLogTruncated = false;
std::chrono::steady_clock::time_point g_runLogStarted;
uint64_t g_rtlsCallCount = 0;
constexpr uint64_t kMaxRunLogBytes = 4ull * 1024 * 1024;
constexpr size_t kMaxRtlsCallNames = 256;
sigjmp_buf g_jmp;
volatile sig_atomic_t g_active = 0;
volatile sig_atomic_t g_sigCaught = 0;
volatile pid_t g_guestTid = 0;
uint64_t g_pc = 0, g_addr = 0, g_lr = 0;
int g_exitCode = 0;
const char* g_reason = nullptr;
// Which image (index into the run order) the process is currently inside, and which one called
// _exit/_abort. An exit ends the whole run, exactly like the real thing, so images that were
// never started are reported as skipped instead of being run behind the guest's back.
volatile int g_currentImage = 0;
volatile int g_exitImage = -1;
volatile bool g_multiRun = false;
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP};
struct sigaction g_old[4];

pid_t tid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

uint64_t pcOf(void* ctx) {
  auto* uc = static_cast<ucontext_t*>(ctx);
#if defined(__aarch64__)
  return uc->uc_mcontext.pc;
#elif defined(__arm__)
  return uc->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
  return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
  (void)uc;
  return 0;
#endif
}

void onSignal(int sig, siginfo_t* si, void* ctx) {
  if (g_active && tid() == g_guestTid) {
    g_sigCaught = sig;
    g_pc = pcOf(ctx);
#if defined(__aarch64__)
    g_lr = static_cast<ucontext_t*>(ctx)->uc_mcontext.regs[30];
#elif defined(__arm__)
    g_lr = static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_lr;
#else
    g_lr = 0;
#endif
    g_addr = reinterpret_cast<uint64_t>(si->si_addr);
    siglongjmp(g_jmp, 1);
  }
  for (int i = 0; i < 4; ++i) {
    if (kSignals[i] != sig) continue;
    struct sigaction& o = g_old[i];
    if ((o.sa_flags & SA_SIGINFO) && o.sa_sigaction) { o.sa_sigaction(sig, si, ctx); return; }
    if (o.sa_handler != SIG_DFL && o.sa_handler != SIG_IGN && o.sa_handler) { o.sa_handler(sig); return; }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

const char* sigName(int s) {
  switch (s) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
  }
}

bool hostCanRunArch(macho::Arch arch) {
#if defined(__aarch64__)
  return arch == macho::Arch::ARM64 || arch == macho::Arch::ARM64e;
#elif defined(__arm__)
  return relinker::isArm32Architecture(arch);
#else
  (void)arch;
  return false;
#endif
}

std::string hostArchError(macho::Arch arch) {
#if defined(__aarch64__)
  if (relinker::isArm32Architecture(arch))
    return "ARM32 guest code requires the ARM32 edition; it cannot execute inside an ARM64 process";
  return "guest execution requires an ARM64 host; this build cannot execute the selected architecture";
#elif defined(__arm__)
  if (arch == macho::Arch::ARM64 || arch == macho::Arch::ARM64e)
    return "ARM64 guest code requires the ARM64 edition; it cannot execute inside an ARM32 process";
  return "guest execution requires an ARM32 host; this build cannot execute the selected architecture";
#else
  (void)arch;
  return "guest execution requires an ARM64 host; this build runs on a different CPU architecture";
#endif
}

bool matchesTrapPc(const relinker::LinkedImage& image, const relinker::Trap& trap,
                   uint64_t pc, int signal) {
  if (trap.addr == pc) return true;
  // A32 BKPT reports the architectural PC after the four-byte instruction on Linux.
  return relinker::isArm32Architecture(image.arch) && signal == SIGTRAP &&
         trap.addr <= UINT64_MAX - 4 && trap.addr + 4 == pc;
}

uint64_t readGuestPointer(const relinker::LinkedImage& image, uint64_t address) {
  if (relinker::isArm32Architecture(image.arch)) {
    uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
  }
  uint64_t value = 0;
  std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
  return value;
}

struct Job {
  const relinker::LinkedImage* li;
  RunResult res;
};

[[maybe_unused]] void* guestThread(void* p) {
  Job* job = static_cast<Job*>(p);
  const relinker::LinkedImage& li = *job->li;
  RunResult& res = job->res;

  // 1. Map the image at its link address and copy bytes in.
  void* m = mmap(reinterpret_cast<void*>(li.loadBase), li.totalSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (m == MAP_FAILED || reinterpret_cast<uint64_t>(m) != li.loadBase) {
    if (m != MAP_FAILED) munmap(m, li.totalSize);
    res.error = "map failed: could not map image at requested base";
    return nullptr;
  }
  std::memcpy(m, li.memory.data(), li.totalSize);
  // 2. Apply segment protections (W^X) and flush the instruction cache.
  for (const auto& r : li.regions) {
    if (r.prot == 0) continue;
    if (mprotect(reinterpret_cast<void*>(r.addr), r.size, static_cast<int>(r.prot & 7)) != 0) {
      res.error = "map failed: mprotect failed for " + r.name;
      munmap(m, li.totalSize);
      return nullptr;
    }
    if (r.code) __builtin___clear_cache(reinterpret_cast<char*>(r.addr), reinterpret_cast<char*>(r.addr + r.size));
  }
  if (!li.entry) { res.error = "image has no entry point"; return nullptr; }

  // 3. Install fault handlers and call into guest code.
  g_guestTid = tid();
  g_sigCaught = 0;
  g_lr = 0;
  g_reason = nullptr;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = onSignal;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &sa, &g_old[i]);

  static char arg0[] = "/var/containers/Bundle/Application/RadekiGuest";
  char* argv[] = {arg0, nullptr};
  char* envp[] = {nullptr};
  char* apple[] = {nullptr};
  using MainFn = int (*)(int, char**, char**, char**);
  using InitFn = void (*)(int, char**, char**, char**);
  if (sigsetjmp(g_jmp, 1) == 0) {
    g_active = 1;
    for (uint64_t slot : li.initPointerSlots) {
      uint64_t fn = readGuestPointer(li, slot);
      if (!fn) continue;
      // Name the constructor in the compat-call trace so a fault inside __GLOBAL__I_* is
      // attributable without a debugger (e.g. __GLOBAL__I_a17 at the MCPE halt site).
      char hex[24];
      std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(fn));
      noteCompatCall("mod_init:" + li.symbolAt(fn).value_or(hex));
      reinterpret_cast<InitFn>(fn)(1, argv, envp, apple);
    }
    int rc = reinterpret_cast<MainFn>(*li.entry)(1, argv, envp, apple);
    g_active = 0;
    res.ran = true;
    res.exitCode = rc;
    finalizeCxx(nullptr);
  } else {
    g_active = 0;
    if (g_sigCaught) {
      res.crashed = true;
      res.signalName = sigName(g_sigCaught);
      res.faultPc = g_pc;
      res.faultAddr = g_addr;
      res.callAddress = g_lr;
      res.callSymbol = li.symbolAt(g_lr).value_or("");
      res.threadId = g_guestTid;
      res.recentCalls = recentCompatCalls();
      for (const auto& t : li.traps)
        if (matchesTrapPc(li, t, g_pc, g_sigCaught)) { res.trapSymbol = t.symbol; res.trapClass = "UNSUPPORTED"; res.resolutionMethod = "trap";
          for (const auto& b : li.imports) if (b.symbol == t.symbol && b.trapIndex == int(t.index)) {
            res.subsystem = b.framework.empty() ? b.dylib : b.framework; break;
          }
        }
    } else {
      res.ran = true;
      res.exitCode = g_exitCode;
      if (g_reason) res.error = g_reason;
    }
  }
  for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &g_old[i], nullptr);
  return nullptr;
}

void writeRunLogLocked(std::string_view text) {
  if (g_runLogFd < 0 || text.empty() || g_runLogTruncated) return;
  const uint64_t available = g_runLogBytes < kMaxRunLogBytes ? kMaxRunLogBytes - g_runLogBytes : 0;
  if (text.size() > available) {
    static constexpr std::string_view marker =
        "\n[radeki] Run log reached its 4 MiB safety limit; later events were omitted.\n";
    const size_t contentBytes = available > marker.size() ? size_t(available - marker.size()) : 0;
    text = text.substr(0, contentBytes);
    // The marker is written below after the remaining content.
    auto writeAll = [](int fd, std::string_view bytes) {
      size_t offset = 0;
      while (offset < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return size_t(0);
        offset += static_cast<size_t>(n);
      }
      return offset;
    };
    g_runLogBytes += writeAll(g_runLogFd, text);
    if (g_runLogBytes + marker.size() <= kMaxRunLogBytes)
      g_runLogBytes += writeAll(g_runLogFd, marker);
    g_runLogTruncated = true;
    return;
  }
  size_t offset = 0;
  while (offset < text.size()) {
    ssize_t n = ::write(g_runLogFd, text.data() + offset, text.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    offset += static_cast<size_t>(n);
    g_runLogBytes += static_cast<size_t>(n);
  }
}

}  // namespace

void beginRunLog(const std::string& path, bool realtimeLogging) {
  endRunLog();
  if (path.empty()) return;
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return;
  struct stat st{};
  uint64_t existingBytes = ::fstat(fd, &st) == 0 && st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
  {
    std::lock_guard<std::mutex> lock(g_runLogMutex);
    g_runLogFd = fd;
    g_runLogBytes = existingBytes;
    g_runLogTruncated = existingBytes >= kMaxRunLogBytes;
    g_runLogStarted = std::chrono::steady_clock::now();
  }
  {
    std::lock_guard<std::mutex> lock(g_rtlsCallsMutex);
    g_rtlsCallNames.clear();
    g_rtlsCallCount = 0;
  }
  g_rtlsEnabled.store(realtimeLogging, std::memory_order_release);
  logRunEvent(realtimeLogging ? "RTLS connected (sampled compatibility calls and frame heartbeats enabled)"
                              : "runtime log connected (RTLS detail disabled)");
}

void endRunLog() {
  g_rtlsEnabled.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(g_runLogMutex);
  if (g_runLogFd >= 0) {
    ::fsync(g_runLogFd);
    ::close(g_runLogFd);
    g_runLogFd = -1;
  }
}

void writeRunLog(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lock(g_runLogMutex);
  writeRunLogLocked(text);
}

void logRunEvent(const std::string& event) {
  if (event.empty()) return;
  uint64_t elapsedMs = 0;
  {
    std::lock_guard<std::mutex> lock(g_runLogMutex);
    if (g_runLogFd < 0) return;
    elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_runLogStarted).count());
    std::string line = "[+" + std::to_string(elapsedMs) + " ms] " + event + "\n";
    writeRunLogLocked(line);
  }
}

bool realtimeLoggingEnabled() {
  return g_rtlsEnabled.load(std::memory_order_acquire);
}

void abortGuest(int code, const char* reason) {
  g_exitCode = code;
  g_reason = reason;
  g_sigCaught = 0;
  g_lr = 0;
  g_exitImage = g_multiRun ? g_currentImage : -1;
  siglongjmp(g_jmp, 1);
}

RunResult run(const relinker::LinkedImage& li) {
  ensureSandboxDirectories();
  if (!hostCanRunArch(li.arch)) {
    RunResult result;
    result.error = hostArchError(li.arch);
    return result;
  }
  std::lock_guard<std::mutex> lock(g_runMutex);
  resetRecentCompatCalls();
  Job job{&li, {}};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8u << 20);
  pthread_t th;
  if (pthread_create(&th, &attr, guestThread, &job) != 0) {
    job.res.error = "could not create guest thread";
  } else {
    pthread_join(th, nullptr);
  }
  pthread_attr_destroy(&attr);
  job.res.output = takeGuestOutput();
  return job.res;
}

namespace {

// Runs every linked image in one address space. Called on the guest thread with the signal
// handlers already installed (see runLoadedImage below).
[[maybe_unused]] void runImagesMulti(MultiRunResult& out, const std::vector<const loader::Entry*>& images) {
  uint64_t anchor = UINT64_MAX, end = 0;
  for (const loader::Entry* e : images) {
    if (!e->linked || e->linkedImage.memory.empty()) continue;
    anchor = std::min(anchor, e->loadBase);
    end = std::max(end, e->loadBase + e->linkedImage.totalSize);
  }
  if (anchor == UINT64_MAX) {
    out.error = "nothing to run: no image linked successfully";
    return;
  }
  uint64_t total = end - anchor;

  // One reservation for the whole set, so every image keeps the slide the loader assigned.
  void* m = mmap(reinterpret_cast<void*>(anchor), total, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (m == MAP_FAILED || reinterpret_cast<uint64_t>(m) != anchor) {
    if (m != MAP_FAILED) munmap(m, total);
    out.error = "map failed: could not reserve the image span at the requested base";
    return;
  }
  for (const loader::Entry* e : images) {
    if (!e->linked || e->linkedImage.memory.empty()) continue;
    std::memcpy(reinterpret_cast<void*>(e->loadBase), e->linkedImage.memory.data(), e->linkedImage.totalSize);
  }

  // Apply each image's protections to the shared span, then flush the icache once per code
  // interval. Everything was written while the mapping was writable, so W^X still holds.
  std::vector<uint64_t> bounds{anchor, anchor + total};
  for (const loader::Entry* e : images) {
    if (!e->linked) continue;
    for (const auto& r : e->linkedImage.regions) {
      if (r.addr < anchor || r.addr + r.size > anchor + total) continue;
      bounds.push_back(r.addr);
      bounds.push_back(r.addr + r.size);
    }
  }
  std::sort(bounds.begin(), bounds.end());
  bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    uint64_t a = bounds[i], b = bounds[i + 1];
    if (a == b) continue;
    uint32_t prot = 0;
    bool covered = false, code = false;
    for (const loader::Entry* e : images) {
      if (!e->linked) continue;
      for (const auto& r : e->linkedImage.regions) {
        if (r.addr <= a && b <= r.addr + r.size) {
          covered = true;
          if (r.prot == 0) { prot = 0; code = false; }
          else { prot |= r.prot & 7; code = code || r.code; }
        }
      }
    }
    int p = covered ? static_cast<int>(prot) : PROT_NONE;
    if (mprotect(reinterpret_cast<void*>(a), b - a, p) != 0) {
      out.error = "map failed: mprotect failed for the image span";
      munmap(m, total);
      return;
    }
    if (code) __builtin___clear_cache(reinterpret_cast<char*>(a), reinterpret_cast<char*>(b));
  }

  static char arg0[] = "/var/containers/Bundle/Application/RadekiGuest";
  char* argv[] = {arg0, nullptr};
  char* envp[] = {nullptr};
  char* apple[] = {nullptr};
  using MainFn = int (*)(int, char**, char**, char**);
  using InitFn = void (*)(int, char**, char**, char**);

  g_currentImage = 0;
  g_exitImage = -1;
  g_multiRun = true;
  if (sigsetjmp(g_jmp, 1) == 0) {
    g_active = 1;
    for (size_t i = 0; i < images.size(); ++i) {
      const loader::Entry* e = images[i];
      ImageRun& ir = out.images[i];
      ir.path = e->path;
      if (!e->linked || e->linkedImage.memory.empty()) {
        ir.skipped = true;
        ir.error = "not started: image did not link";
        continue;
      }
      auto runInits = [&](const relinker::LinkedImage& lim) {
        for (uint64_t slot : lim.initPointerSlots) {
          uint64_t fn = readGuestPointer(lim, slot);
          if (!fn) continue;
          char hex[24];
          std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(fn));
          noteCompatCall("mod_init:" + lim.symbolAt(fn).value_or(hex));
          reinterpret_cast<InitFn>(fn)(1, argv, envp, apple);
        }
      };
      if (!e->linkedImage.entry) {
        ir.skipped = true;
        ir.error = "no entry point (not an executable image); its initializers still ran";
        runInits(e->linkedImage);
        continue;
      }
      g_currentImage = static_cast<int>(i);
      g_exitImage = -1;
      runInits(e->linkedImage);
      int rc = reinterpret_cast<MainFn>(*e->linkedImage.entry)(1, argv, envp, apple);
      ir.ran = true;
      ir.exitCode = rc;
      out.ran = true;
      out.exitCode = rc;
    }
    finalizeCxx(nullptr);
    g_active = 0;
    g_multiRun = false;
  } else {
    g_active = 0;
    g_multiRun = false;
    int which = g_currentImage;
    if (which >= 0 && static_cast<size_t>(which) < out.images.size()) {
      ImageRun& ir = out.images[which];
      if (g_sigCaught) {
        out.crashed = true;
        ir.crashed = true;
        ir.signalName = sigName(g_sigCaught);
        ir.faultPc = g_pc;
        ir.faultAddr = g_addr;
        for (const auto& e : images)
          if (e->linked)
            for (const auto& t : e->linkedImage.traps)
              if (matchesTrapPc(e->linkedImage, t, g_pc, g_sigCaught)) ir.trapSymbol = t.symbol;
      } else {
        ir.ran = true;
        ir.exitCode = g_exitCode;
        out.ran = true;
        out.exitCode = g_exitCode;
        if (g_reason) ir.error = g_reason;
      }
    }
    // Everything after the image that ended the process never started.
    for (size_t i = size_t(which) + 1; i < out.images.size(); ++i) {
      out.images[i].path = images[i]->path;
      out.images[i].skipped = true;
      out.images[i].error = "not started: the process exited during an earlier image";
    }
  }
}

}  // namespace

MultiRunResult runLoadedImage(loader::Registry& reg, const loader::Options& opt) {
  ensureSandboxDirectories();
  MultiRunResult out;
  loader::LoadResult lr = reg.linkAll(opt);
  out.error = lr.errors.empty() ? "" : lr.errors.front();
  std::vector<const loader::Entry*> images = lr.order;
  out.images.resize(images.size());
  // Report which imports run on logging stubs instead of implementations (honest reporting:
  // the list is capped but the count is exact).
  for (const auto* e : images)
    if (e->linked)
      for (const auto& b : e->linkedImage.imports)
        if (b.dispatchStub) {
          ++out.dispatchStubs;
          if (out.stubbed.size() < 64) out.stubbed.push_back(b.symbol);
        }
  if (images.empty()) {
    if (out.error.empty()) out.error = "no images to run";
    return out;
  }
  bool hostCanRunAll = true;
  for (size_t i = 0; i < images.size(); ++i) {
    out.images[i].path = images[i]->path;
    const macho::Arch arch = images[i]->linked ? images[i]->linkedImage.arch
                                               : images[i]->image ? images[i]->image->arch : macho::Arch::Unknown;
    if (!hostCanRunArch(arch)) {
      hostCanRunAll = false;
      out.images[i].skipped = true;
      out.images[i].error = hostArchError(arch);
      if (out.error.empty()) out.error = out.images[i].error;
    }
  }
  if (!hostCanRunAll) return out;
  {
    std::lock_guard<std::mutex> lock(g_runMutex);
    struct Job {
      MultiRunResult* out;
      const std::vector<const loader::Entry*>* images;
    } job{&out, &images};

    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    g_guestTid = tid();
    g_sigCaught = 0;
  g_lr = 0;
    g_reason = nullptr;
    for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &sa, &g_old[i]);
    runImagesMulti(*job.out, *job.images);
    for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &g_old[i], nullptr);
  }
  out.output = takeGuestOutput();
  return out;
}

RunResult runImage(const macho::Image& img, bool useStubs) {
  RunResult none;
  if (img.hasTLS) { none.error = "thread-local variables are not supported yet"; return none; }
  HostRuntime host;
  std::optional<relinker::CompatRegistry> plainRegistry;
  const relinker::ImportResolver* resolver = nullptr;
  if (useStubs) {
    host = makeHostRuntime();
    resolver = &host.registry;
  } else {
    plainRegistry.emplace(makeCompatRegistry());
    resolver = &*plainRegistry;
  }
  std::vector<uint64_t> candidates{img.textBase()};
  if (relinker::isArm32Architecture(img.arch))
    candidates.insert(candidates.end(), {0x10000000, 0x20000000, 0x40000000, 0x60000000});
  else
    candidates.insert(candidates.end(), {0x300000000ull, 0x500000000ull, 0x900000000ull});
  const uint64_t pageSize = relinker::pageSizeForArch(img.arch);
  RunResult last;
  for (uint64_t base : candidates) {
    if (base % pageSize || (relinker::isArm32Architecture(img.arch) && base > UINT32_MAX)) continue;
    relinker::LinkOptions o;
    o.loadBase = base;
    o.resolver = resolver;
    if (useStubs) o.stubFactory = host.stubFactory;
    try {
      auto li = relinker::link(img, o);
      last = run(li);
      for (const auto& b : li.imports) last.dispatchStubs += b.dispatchStub;
    } catch (const relinker::LinkError& e) {
      last = RunResult{};
      last.error = std::string("link failed: ") + e.what();
      return last;
    }
    if (last.error.rfind("map failed", 0) != 0) return last;
  }
  return last;
}

// --- host runtime layer --------------------------------------------------------------------

HostRuntime::~HostRuntime() { delete dlsym; }
HostRuntime::HostRuntime(HostRuntime&& o) noexcept
    : registry(std::move(o.registry)), dlsym(std::exchange(o.dlsym, nullptr)),
      stubFactory(std::move(o.stubFactory)) {
  registry.chainNext(dlsym);  // rewire the chained pointer into the moved-in resolver
}
HostRuntime& HostRuntime::operator=(HostRuntime&& o) noexcept {
  if (this != &o) {
    delete dlsym;
    registry = std::move(o.registry);
    dlsym = std::exchange(o.dlsym, nullptr);
    stubFactory = std::move(o.stubFactory);
    registry.chainNext(dlsym);
  }
  return *this;
}

HostRuntime makeHostRuntime() {
  ensureSandboxDirectories();
  HostRuntime h;
  h.registry = makeCompatRegistry();
  registerFrameworkStubs(h.registry);   // UIKit / EAGL / Foundation / CoreFoundation dummies
  h.dlsym = new HostCxxResolver();      // dlsym onto the host libc++ (libc++_shared on Android)
  h.registry.chainNext(h.dlsym);
  h.stubFactory = makeDispatchStubFactory();
  return h;
}

}  // namespace radeki::runtime

namespace radeki::runtime {
namespace {
constexpr size_t kRecentCallCapacity = 32;
constexpr size_t kRecentCallLength = 128;
std::mutex callsMutex;
std::array<std::array<char, kRecentCallLength>, kRecentCallCapacity> calls{};
size_t callStart = 0;
size_t callCount = 0;

void recordCompatCall(const char* name) {
  if (!name) name = "";
  std::lock_guard<std::mutex> lock(callsMutex);
  size_t slot;
  if (callCount < kRecentCallCapacity) {
    slot = (callStart + callCount++) % kRecentCallCapacity;
  } else {
    slot = callStart;
    callStart = (callStart + 1) % kRecentCallCapacity;
  }
  auto& dest = calls[slot];
  size_t len = 0;
  while (len + 1 < dest.size() && name[len] != '\0') ++len;
  if (len) std::memcpy(dest.data(), name, len);
  dest[len] = '\0';
}

void recordRtlsCompatCall(const char* name) {
  if (!name || !realtimeLoggingEnabled()) return;
  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lock(g_rtlsCallsMutex);
    ++g_rtlsCallCount;
    if (g_rtlsCallNames.size() < kMaxRtlsCallNames)
      shouldLog = g_rtlsCallNames.emplace(name).second;
    else
      shouldLog = g_rtlsCallCount % 4096 == 0;
  }
  if (shouldLog) logRunEvent(std::string("compat call: ") + name);
}
}  // namespace

void noteCompatCall(const char* name) {
  recordCompatCall(name);
  recordRtlsCompatCall(name);
}
void noteCompatCall(const std::string& name) { noteCompatCall(name.c_str()); }

std::vector<std::string> recentCompatCalls() {
  std::lock_guard<std::mutex> lock(callsMutex);
  std::vector<std::string> result;
  result.reserve(callCount);
  for (size_t i = 0; i < callCount; ++i)
    result.emplace_back(calls[(callStart + i) % kRecentCallCapacity].data());
  return result;
}

void resetRecentCompatCalls() {
  std::lock_guard<std::mutex> lock(callsMutex);
  callStart = 0;
  callCount = 0;
}
std::string describeResult(const RunResult& r) {
  std::ostringstream o;
  if(r.crashed) o << (r.signalName.empty()?"crash":r.signalName);
  else if(!r.error.empty()) o << r.error;
  else if(r.ran) o << "guest returned " << r.exitCode;
  else o << "not run";
  if(!r.trapSymbol.empty()) {
    o << " - unsupported symbol: " << r.trapSymbol;
    o << " (subsystem: " << (r.subsystem.empty()?"unknown":r.subsystem) << ")";
    o << " (resolution: " << (r.resolutionMethod.empty()?"trap":r.resolutionMethod) << ")";
  }
  if(r.faultPc) o << ", guest PC 0x" << std::hex << r.faultPc << std::dec;
  if(r.callAddress) o << ", called from " << (r.callSymbol.empty()?"unknown":r.callSymbol) << " (0x" << std::hex << r.callAddress << std::dec << ")";
  else if(r.crashed) o << ", caller unknown";
  if(!r.image.empty()) o << ", image " << r.image;
  if(r.threadId) o << ", thread " << r.threadId;
  if(!r.recentCalls.empty()) {o << "; recent compat calls: ";for(size_t i=0;i<r.recentCalls.size();++i){if(i)o << " -> ";o << r.recentCalls[i];}}
  if(r.dispatchStubs) o << "; " << r.dispatchStubs << " imports ran on logging no-op stubs";
  return o.str();
}
}
