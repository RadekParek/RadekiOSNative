// Generic dynamic stub dispatcher: the runtime answer to "446 unbound imports must not become
// BRK/SIGTRAP traps".
//
// When the linker runs with a StubFactory (loader::Options::stubFactory), every unresolved
// strong import is bound to a host-side object produced here instead of a trap stub:
//
//   * code imports  -> a 32-byte AArch64 trampoline emitted in an RW->RX arena:
//                        LDR x9,  =StubRecord*     ; which symbol was called
//                        LDR x16, =dispatch entry  ; shared dispatcher
//                        BR  x16
//                      The dispatcher logs the symbol (once, then sampled), and returns a safe
//                      default: 0/NULL/nil, 1/YES, the caller's x0 (id pass-through for the
//                      objc retain family / objc_msgSend), or an empty SEL pointer.
//   * data imports  -> dummy objects: zeroed objc-class-shaped blocks for _OBJC_CLASS_$_ /
//                      _OBJC_METACLASS_$_ symbols (labelled with the class name), and labelled
//                      zero blocks for constants (_kCF..., ...Notification, NSConcrete*Block).
//
// Honesty contract: a dispatch stub is never reported as resolved. The import stays in
// LoadResult::unresolved (with `stub` set), stays classified NOOP_STUB, and every first call
// is logged. Nothing here pretends to implement a framework.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "relinker/relinker.h"

namespace radeki::runtime {

enum class StubReturn : uint8_t {
  Zero,      // 0 / NULL / nil -- the safe default
  One,       // YES
  Arg0,      // pass the caller's x0 through (objc_retain family, objc_msgSend, sel_registerName)
  EmptySel,  // pointer to a static empty C string (SEL-like)
};

struct StubRecord {
  std::string symbol;
  std::string dylib;
  StubReturn ret = StubReturn::Zero;
  std::atomic<uint64_t> calls{0};
};

// Enables/disables sampled missing-import log output. The default is on for native callers;
// Android can turn it off to reduce guest-call overhead without changing stub return values.
void setStubCallLogging(bool enabled);

// Returns the safe-default class for a symbol name (table + heuristics, see the .cpp).
StubReturn classifyStubReturn(const std::string& symbol);
// True when the symbol is a data import (class objects, ObjC block classes, constants).
bool stubLooksLikeData(const std::string& symbol);

// The C half of the dispatcher; also called directly by the tests. `originalX0` is the
// caller's x0 (first argument register), needed for Arg0 pass-through decisions.
extern "C" uint64_t radekiStubDispatchC(StubRecord* rec, uint64_t originalX0);
// Address the trampolines branch to (the naked AArch64 entry, or a fallback elsewhere).
uint64_t stubDispatchEntryAddress();

class StubArena {
 public:
  static StubArena& instance();

  // Emits (or reuses) the trampoline for a symbol; returns its host address. Deterministic:
  // the same (symbol, dylib) always yields the same address, so the loader's two link passes
  // stay consistent.
  uint64_t makeCodeStub(const std::string& symbol, const std::string& dylib);
  // Emits (or reuses) a dummy data object; returns its host address. `label` is the class or
  // constant name for diagnostics ("UIView", "kCFAllocatorDefault", ...).
  uint64_t makeDataObject(const std::string& symbol, const std::string& dylib, const std::string& label);

  const StubRecord* recordForStubAddress(uint64_t trampolineAddr) const;
  std::string dataObjectLabel(uint64_t objectAddr) const;
  size_t codeStubCount() const;
  size_t dataObjectCount() const;

 private:
  StubArena() = default;
  struct Impl;
  static Impl& impl();
};

// The factory handed to relinker::LinkOptions / loader::Options: code symbols become
// trampolines, data symbols become dummy objects.
relinker::StubFactory makeDispatchStubFactory();

}  // namespace radeki::runtime
