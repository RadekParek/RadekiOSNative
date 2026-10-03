// libc++.1.dylib -> host C++ runtime forwarding.
//
// Apple's libc++ and the Android NDK's libc++ (libc++_shared.so / c++_static) are the same
// upstream project and share the std::__1 inline namespace, the Itanium ABI and the
// basic_string storage layout. Two forwarding layers hook into the dyld relocation pass via
// the loader's fallback resolver:
//
//  1. An explicit mapping table (cxxForwardTable): Apple's exact mangled symbol names mapped
//     to written wrappers that speak the guest's pointer-width libc++ ABI directly (no host
//     std::string reinterpret -- the host may be libstdc++, which has a different layout).
//     basic_string allocation/initialization and ios_base::Init are covered so common static
//     global constructors (__GLOBAL__I_*) can complete without hitting trap instructions.
//
//  2. HostCxxResolver: a dlsym(RTLD_DEFAULT) pass-through for remaining _Z-mangled imports
//     requested from Apple's C++ dylibs. On an Android device this finds the identical
//     std::__1 symbols inside the NDK libc++ linked into this process.
//
// Doctrine: exceptions/unwinding symbols stay unregistered on purpose -- a silent no-op
// __cxa_throw would corrupt the guest far away from the call site.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "relinker/relinker.h"

namespace radeki::runtime {

struct CxxForwardEntry {
  std::string mangled;  // Apple's mangled export name (as imported from libc++.1.dylib)
  uint64_t impl = 0;    // host address of the wrapper / data object
  std::string what;     // human-readable C++ spelling, for reports
  bool data = false;    // true: impl is the address of storage, not a function
};

// The explicit mapping table. Stable for the process lifetime.
const std::vector<CxxForwardEntry>& cxxForwardTable();
// Substitute 32-bit unsigned-long size_t type codes in the supported std::string symbols.
// Useful for Apple ARMv7 imports, where std::string::size_type has a 32-bit ABI spelling.
std::string arm32ManglingAlias(const std::string& mangled);
std::optional<uint64_t> lookupCxxForward(const std::string& mangled);
// Registers every table entry into a compat registry (classification ANDROID_BACKEND,
// framework "libc++"). Called from makeCompatRegistry().
void addCxxForwarding(relinker::CompatRegistry& reg);

// True for install names of Apple's C++ runtime dylibs (libc++.1.dylib, libc++abi.dylib).
bool isAppleCxxDylib(const std::string& installName);

// dlsym pass-through onto the host C++ runtime (libc++_shared on Android). Only answers
// _Z-mangled symbols requested from Apple C++ dylibs (or flat lookups). Hits are cached.
class HostCxxResolver : public relinker::ImportResolver {
 public:
  std::optional<uint64_t> resolve(const std::string& symbol, const std::string& dylib) const override;
  std::optional<compat::CompatEntry> describe(const std::string& symbol, const std::string& dylib) const override;
  size_t cachedHits() const;

 private:
  struct Cache;
  static Cache& cache();
};

}  // namespace radeki::runtime
