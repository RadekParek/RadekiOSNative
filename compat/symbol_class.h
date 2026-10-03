#pragma once
#include <string>
namespace radeki::compat {
// NoopStub: bound to a logging no-op dispatcher (code) or a dummy object (data). The import is
// still reported as unresolved everywhere; the stub only keeps the guest running past the call.
enum class SymbolClass { Unsupported, HostDirect, CompatibilityShim, AndroidBackend, IOSFramework, WeakOptional, GuestImage, NoopStub };
inline const char* symbolClassName(SymbolClass c) {
  switch (c) {
    case SymbolClass::Unsupported: return "UNSUPPORTED";
    case SymbolClass::HostDirect: return "HOST_DIRECT";
    case SymbolClass::CompatibilityShim: return "COMPATIBILITY_SHIM";
    case SymbolClass::AndroidBackend: return "ANDROID_BACKEND";
    case SymbolClass::IOSFramework: return "IOS_FRAMEWORK";
    case SymbolClass::WeakOptional: return "WEAK_OPTIONAL";
    case SymbolClass::GuestImage: return "GUEST_IMAGE";
    case SymbolClass::NoopStub: return "NOOP_STUB";
  }
  return "UNSUPPORTED";
}
struct CompatEntry { SymbolClass cls = SymbolClass::Unsupported; std::string framework, method; };
}
