#include "runtime/compat_cxx.h"
#include "runtime/runtime.h"
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

namespace radeki::runtime {
namespace {
struct ExitEntry { void (*fn)(void*); void* arg; void* dso; bool run; };
std::mutex exitMutex;
std::vector<ExitEntry> exits;
std::mutex guardMutex;
std::condition_variable guardCv;
std::unordered_map<uint64_t*, std::thread::id> activeGuards;
uint64_t addr(auto f) { return reinterpret_cast<uint64_t>(f); }
[[noreturn]] void pureVirtual() { abortGuest(134,"pure or deleted virtual call"); }
void* newScalar(size_t n) { return ::operator new(n); }
void* newArray(size_t n) { return ::operator new[](n); }
void* newNoThrow(size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void* newArrayNoThrow(size_t n, const std::nothrow_t&) noexcept { return ::operator new[](n, std::nothrow); }
void deleteScalar(void* p) noexcept { ::operator delete(p); }
void deleteArray(void* p) noexcept { ::operator delete[](p); }
void deleteSized(void* p, size_t n) noexcept { ::operator delete(p, n); }
void deleteArraySized(void* p, size_t n) noexcept { ::operator delete[](p, n); }
}
int registerCxxExit(void (*fn)(void*), void* arg, void* dso) {
  if (!fn) return -1;
  try { std::lock_guard<std::mutex> lock(exitMutex); exits.push_back({fn,arg,dso,false}); }
  catch (...) { return -1; }
  return 0;
}
void finalizeCxx(void* dso) {
  // Mark one at a time, then call outside the lock: callbacks can register or finalize.
  for (;;) {
    ExitEntry next{};
    {
      std::lock_guard<std::mutex> lock(exitMutex);
      for (auto it = exits.rbegin(); it != exits.rend(); ++it)
        if (!it->run && (!dso || it->dso == dso)) { it->run = true; next = *it; break; }
    }
    if (!next.fn) break;
    next.fn(next.arg);
  }
}
int acquireCxxGuard(uint64_t* guard) {
  if (!guard) return 0;
  std::unique_lock<std::mutex> lock(guardMutex);
  for (;;) {
    if (*guard & 1) return 0;
    auto it = activeGuards.find(guard);
    if (it == activeGuards.end()) { activeGuards.emplace(guard, std::this_thread::get_id()); return 1; }
    if (it->second == std::this_thread::get_id()) std::abort(); // recursive static initialization
    guardCv.wait(lock);
  }
}
void releaseCxxGuard(uint64_t* guard) {
  std::lock_guard<std::mutex> lock(guardMutex);
  if (guard && activeGuards.erase(guard)) *guard |= 1;
  guardCv.notify_all();
}
void abortCxxGuard(uint64_t* guard) {
  std::lock_guard<std::mutex> lock(guardMutex);
  activeGuards.erase(guard);
  guardCv.notify_all();
}
void addCxxCompat(relinker::CompatRegistry& r) {
  auto add = [&](const char* name, uint64_t fn) { r.add(name, fn, {compat::SymbolClass::CompatibilityShim, "libc++abi", "cxx_shim"}); };
  add("___cxa_atexit", addr(registerCxxExit));
  add("___cxa_thread_atexit_impl", addr(registerCxxExit)); // process-wide, not thread-local
  add("___cxa_finalize", addr(finalizeCxx));
  add("___cxa_guard_acquire", addr(acquireCxxGuard));
  add("___cxa_guard_release", addr(releaseCxxGuard));
  add("___cxa_guard_abort", addr(abortCxxGuard));
  add("___cxa_pure_virtual", addr(pureVirtual));
  add("___cxa_deleted_virtual", addr(pureVirtual));
  add("__Znwm", addr(newScalar)); add("__Znam", addr(newArray));
  add("__ZdlPv", addr(deleteScalar)); add("__ZdaPv", addr(deleteArray));
  add("__ZdlPvm", addr(deleteSized)); add("__ZdaPvm", addr(deleteArraySized));
  add("__ZnwmRKSt9nothrow_t", addr(newNoThrow));
  add("__ZnamRKSt9nothrow_t", addr(newArrayNoThrow));
}
}
