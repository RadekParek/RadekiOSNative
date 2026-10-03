// Generic dynamic stub dispatcher implementation. See stub_dispatch.h for the contract.
#include "runtime/stub_dispatch.h"
#include "runtime/runtime.h"

#include <sys/mman.h>

#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace radeki::runtime {
namespace {

std::atomic<bool> g_logStubCalls{true};

std::string leafName(const std::string& p) {
  size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

constexpr uint32_t kLdrX9Lit16 = 0x58000089;   // LDR x9,  [pc, #16]  -> StubRecord*
constexpr uint32_t kLdrX16Lit24 = 0x580000B0;  // LDR x16, [pc, #24]  -> dispatch entry
constexpr uint32_t kBrX16 = 0xD61F0200;        // BR  x16
constexpr uint32_t kNop = 0xD503201F;
constexpr size_t kTrampolineBytes = 32;        // 4 words + two 8-byte literals
constexpr size_t kChunkBytes = 64 * 1024;

// objc-class-shaped dummy block for _OBJC_CLASS_$_ / _OBJC_METACLASS_$_ and a labelled zero
// block for everything else data-shaped. 64 bytes: real class objects are larger, but nothing
// here ever dispatches through one -- the label exists so crash reports can name the symbol.
struct DummyObject {
  uint64_t isa = 0, superclass = 0, cache = 0, vtable = 0, dataBits = 0;
  char label[24] = {};
};
static_assert(sizeof(DummyObject) == 64, "dummy object stays one cache line");

const char* retKindName(StubReturn r) {
  switch (r) {
    case StubReturn::Zero: return "0/NULL/nil";
    case StubReturn::One: return "YES/1";
    case StubReturn::Arg0: return "x0 pass-through";
    case StubReturn::EmptySel: return "empty SEL";
  }
  return "0";
}

}  // namespace

StubReturn classifyStubReturn(const std::string& symbol) {
  // Pass the receiver/object through: the objc memory-management family and message send must
  // not turn live objects into nil mid-chain, or the startup loop dies one call later.
  static const char* const passthrough[] = {
      "_objc_retain", "_objc_retainBlock", "_objc_retainAutorelease",
      "_objc_autorelease", "_objc_autoreleaseReturnValue",
      "_objc_retainAutoreleasedReturnValue", "_objc_unsafeClaimAutoreleasedReturnValue",
      "_objc_loadWeakRetained", "_objc_rootRetain", "_objc_msgSend",
      "_objc_msgSendSuper", "_objc_msgSendSuper2", "_objc_msgSend_stret",
      "_CFRetain",
  };
  for (const char* n : passthrough)
    if (symbol == n) return StubReturn::Arg0;
  // Non-const std::string member functions take `this` in x0 and commonly return `basic_string&`
  // (or `this` from C1/C2/D1/D2); passing x0 through prevents null-dereference faults if an
  // unmapped overload reaches the fallback stub.
  if (symbol.rfind("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE", 0) == 0)
    return StubReturn::Arg0;
  // SEL-returning helpers: an empty C string is a legal, non-null selector spelling "".
  static const char* const sels[] = {"_sel_registerName", "_sel_getUid", "_NSSelectorFromString", "_sel_getName"};
  for (const char* n : sels)
    if (symbol == n) return StubReturn::EmptySel;
  return StubReturn::Zero;
}

bool stubLooksLikeData(const std::string& symbol) {
  auto starts = [&](const char* p) { return symbol.rfind(p, 0) == 0; };
  auto ends = [&](const char* p) {
    size_t n = std::strlen(p);
    return symbol.size() >= n && symbol.compare(symbol.size() - n, n, p) == 0;
  };
  if (starts("_OBJC_CLASS_$_") || starts("_OBJC_METACLASS_$_")) return true;      // class objects
  if (starts("_NSConcreteStackBlock") || starts("_NSConcreteGlobalBlock") ||
      starts("_NSConcreteMallocBlock")) return true;                              // block classes
  if (starts("_kCF") || starts("_kCG") || starts("_kCA") || starts("_kEAGL") ||
      starts("_kCL") || starts("_kUT") || starts("_kSec") || starts("_kAudio") ||
      starts("_kIO") || starts("_kSC")) return true;                              // Core* constants
  if (starts("_UIApplicationLaunchOptions") || starts("_UIWindowLevel") ||
      starts("_UIKeyboard") || (starts("_UIDevice") && ends("Notification"))) return true;
  if (ends("Notification") || ends("NotificationName") || ends("Exception")) return true;
  if (symbol == "_NSFoundationVersionNumber" || symbol == "_NSAppKitVersionNumber" ||
      symbol == "_UIApplicationDidEnterBackgroundNotification") return true;
  return false;
}

// --- the dispatcher ------------------------------------------------------------------------

extern "C" uint64_t radekiStubDispatchC(StubRecord* rec, uint64_t originalX0) {
  if (!rec) return 0;
  if (g_logStubCalls.load(std::memory_order_relaxed)) {
    uint64_t n = rec->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log the first hit of every symbol, then sample: a hot missing import must not flood the
    // guest output ring buffer (or logcat) into uselessness.
    if (n == 1 || (n % 4096) == 0) {
      noteCompatCall("stub:" + leafName(rec->dylib) + ":" + rec->symbol);
      appendGuestOutput("[radeki-stub] " + rec->symbol + " <" + leafName(rec->dylib) + "> call #" +
                        std::to_string(n) + " -> " + retKindName(rec->ret) + "\n");
    }
  }
  static const char kEmptySel[] = "";
  switch (rec->ret) {
    case StubReturn::One: return 1;
    case StubReturn::Arg0: return originalX0;
    case StubReturn::EmptySel: return reinterpret_cast<uint64_t>(kEmptySel);
    case StubReturn::Zero: break;
  }
  return 0;
}

#if defined(__aarch64__)
// The trampoline BRs here with the StubRecord* in x9 (x9-x15 and x16-x17 are caller-saved
// scratch under AAPCS64, so the guest cannot expect them preserved across the call). The
// guest's own x0 is preserved for Arg0 pass-through; x29/x30 are saved across the C call so
// the guest's frame chain and return address survive; d0 is zeroed so float-returning stubs
// report 0.0 instead of garbage. x19-x29/d8-d15 stay intact by the C calling convention.
extern "C" void radeki_stub_dispatch_entry();
asm(R"asm(
  .globl radeki_stub_dispatch_entry
  .type radeki_stub_dispatch_entry, %function
radeki_stub_dispatch_entry:
  mov x1, x0
  mov x0, x9
  stp x29, x30, [sp, #-16]!
  bl radekiStubDispatchC
  ldp x29, x30, [sp], #16
  mov x9, x0
  mov x1, xzr
  fmov d0, xzr
  mov x0, x9
  ret
)asm");
uint64_t stubDispatchEntryAddress() { return reinterpret_cast<uint64_t>(&radeki_stub_dispatch_entry); }
#else
// Non-AArch64 hosts never execute guest code; the trampolines are only inspected by tests.
// The literal still needs a valid address, so hand it the C dispatcher directly.
static uint64_t fallbackEntry(StubRecord* rec, uint64_t x0) { return radekiStubDispatchC(rec, x0); }
uint64_t stubDispatchEntryAddress() { return reinterpret_cast<uint64_t>(&fallbackEntry); }
#endif

// --- the arena -----------------------------------------------------------------------------

struct StubArena::Impl {
  std::mutex mu;
  std::map<std::pair<std::string, std::string>, uint64_t> byKey;  // code stubs + data objects
  std::deque<StubRecord> records;                                 // addresses stay valid
  std::deque<std::unique_ptr<DummyObject>> objects;               // addresses stay valid
  struct Chunk { uint8_t* base = nullptr; size_t used = 0; bool executable = false; };
  std::vector<Chunk> chunks;

  uint8_t* alloc(size_t bytes) {
    for (auto& c : chunks)
      if (c.used + bytes <= kChunkBytes) {
        uint8_t* p = c.base + c.used;
        c.used += bytes;
        if (c.executable) {  // new code lands in an already-executable chunk: reopen, write, reseal
          mprotect(c.base, kChunkBytes, PROT_READ | PROT_WRITE);
          c.executable = false;
        }
        return p;
      }
    void* m = mmap(nullptr, kChunkBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return nullptr;
    chunks.push_back({static_cast<uint8_t*>(m), bytes, false});
    return chunks.back().base;
  }
  void seal(Chunk& c) {
    if (c.executable) return;
    mprotect(c.base, kChunkBytes, PROT_READ | PROT_EXEC);  // W^X: written while RW, sealed RX
    __builtin___clear_cache(reinterpret_cast<char*>(c.base), reinterpret_cast<char*>(c.base + c.used));
    c.executable = true;
  }
  bool owns(uint64_t addr) const {
    for (const auto& c : chunks) {
      uint64_t b = reinterpret_cast<uint64_t>(c.base);
      if (addr >= b && addr + kTrampolineBytes <= b + c.used) return true;
    }
    return false;
  }
  ~Impl() {
    for (auto& c : chunks) munmap(c.base, kChunkBytes);
  }
};

StubArena::Impl& StubArena::impl() {
  static Impl i;
  return i;
}

StubArena& StubArena::instance() {
  static StubArena a;
  return a;
}

uint64_t StubArena::makeCodeStub(const std::string& symbol, const std::string& dylib) {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  auto key = std::make_pair(symbol, dylib);
  auto it = im.byKey.find(key);
  if (it != im.byKey.end()) return it->second;  // deterministic across both link passes

  // StubRecord holds a std::atomic (non-movable), so construct it in place.
  im.records.emplace_back();
  StubRecord* rec = &im.records.back();
  rec->symbol = symbol;
  rec->dylib = dylib;
  rec->ret = classifyStubReturn(symbol);

  uint8_t* p = im.alloc(kTrampolineBytes);
  if (!p) return 0;
  uint32_t words[4] = {kLdrX9Lit16, kLdrX16Lit24, kBrX16, kNop};
  std::memcpy(p, words, sizeof words);
  uint64_t recAddr = reinterpret_cast<uint64_t>(rec);
  uint64_t entry = stubDispatchEntryAddress();
  std::memcpy(p + 16, &recAddr, 8);
  std::memcpy(p + 24, &entry, 8);
  for (auto& c : im.chunks) {  // seal the chunk that got the code (no-op for untouched ones)
    uint64_t b = reinterpret_cast<uint64_t>(c.base);
    if (reinterpret_cast<uint64_t>(p) >= b && reinterpret_cast<uint64_t>(p) < b + kChunkBytes) im.seal(c);
  }
  uint64_t addr = reinterpret_cast<uint64_t>(p);
  im.byKey.emplace(std::move(key), addr);
  return addr;
}

uint64_t StubArena::makeDataObject(const std::string& symbol, const std::string& dylib, const std::string& label) {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  auto key = std::make_pair(symbol, dylib);
  auto it = im.byKey.find(key);
  if (it != im.byKey.end()) return it->second;
  auto obj = std::make_unique<DummyObject>();
  std::snprintf(obj->label, sizeof obj->label, "%s", label.c_str());
  uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
  im.objects.push_back(std::move(obj));
  im.byKey.emplace(std::move(key), addr);
  return addr;
}

const StubRecord* StubArena::recordForStubAddress(uint64_t addr) const {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  if (!im.owns(addr)) return nullptr;
  uint64_t recAddr = 0;
  std::memcpy(&recAddr, reinterpret_cast<const void*>(addr + 16), 8);
  return reinterpret_cast<const StubRecord*>(recAddr);
}

std::string StubArena::dataObjectLabel(uint64_t addr) const {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  for (const auto& o : im.objects)
    if (reinterpret_cast<uint64_t>(o.get()) == addr) return o->label;
  return "";
}

size_t StubArena::codeStubCount() const {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  return im.records.size();
}

size_t StubArena::dataObjectCount() const {
  Impl& im = impl();
  std::lock_guard<std::mutex> lock(im.mu);
  return im.objects.size();
}

// --- the factory ---------------------------------------------------------------------------

relinker::StubFactory makeDispatchStubFactory() {
  return [](const std::string& symbol, const std::string& dylib) -> uint64_t {
    StubArena& arena = StubArena::instance();
    if (stubLooksLikeData(symbol)) {
      std::string label = symbol;
      for (const char* p : {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_"}) {
        if (label.rfind(p, 0) == 0) { label = label.substr(std::strlen(p)); break; }
      }
      return arena.makeDataObject(symbol, dylib, label);
    }
    return arena.makeCodeStub(symbol, dylib);
  };
}

void setStubCallLogging(bool enabled) {
  g_logStubCalls.store(enabled, std::memory_order_relaxed);
}

}  // namespace radeki::runtime
