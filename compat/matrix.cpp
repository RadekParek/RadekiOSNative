#include "compat/matrix.h"
#include <algorithm>
namespace radeki::compat {
const std::vector<Capability>& capabilityMatrix() {
  static const std::vector<Capability> rows = {
    {"native_executable","Native executable","loader",Status::Implemented,"Matching native ARM64 or ARMv7 process required"},
    {"macho_loading","Mach-O loading","loader",Status::Implemented,"Supported Mach-O structures"},
    {"relocations","Relocations","relinker",Status::Implemented,"Supported rebases and binds"},
    {"multi_image_loading","Multi-image loading","loader",Status::Implemented,"Dependency ordering and exports"},
    {"cxx_runtime","C++ runtime","libc++abi",Status::Partial,"Exceptions and unwinding unsupported"},
    {"objc_runtime","Objective-C runtime","Objective-C",Status::Partial,"Metadata reading only; no dispatch"},
    {"bundle_filesystem","Bundle filesystem","filesystem",Status::Partial,"Not a complete iOS sandbox"},
    {"ipa_metadata","IPA metadata","IPA",Status::Partial,"Not all bundle variants"},
    {"crash_diagnostics","Crash diagnostics","runtime",Status::Partial,"Limited signal context"},
    {"foundation","Foundation","Foundation",Status::Unsupported,"A few startup/path shims only; no Foundation object runtime"},
    {"corefoundation","CoreFoundation","CoreFoundation",Status::Unsupported,"A few C shims only; no CoreFoundation collections or run loop"},
    {"uikit","UIKit","UIKit",Status::Unsupported,"Startup shims only; no UIKit view, event, rendering, or application-loop backend"},
    {"opengles","OpenGL ES","OpenGL ES",Status::Unsupported,"Partial GLES2 forwarding; no working EAGL surface/display integration"},
    {"metal","Metal","Metal",Status::Unsupported,"No Metal backend"},
    {"vulkan","Vulkan","Vulkan",Status::Unsupported,"No Vulkan backend"},
    {"audio","Audio","Audio",Status::Unsupported,"No audio backend"},
    {"input","Input","Input",Status::Unsupported,"No input backend"},
    {"networking","Networking","Networking",Status::Unsupported,"No networking backend"},
    {"arm64e_pac","arm64e PAC","CPU",Status::Unsupported,"No PAC emulation"},
    {"armv7_aot","ARMv7 guest edition","CPU",Status::Unsupported,"ARMv7 guests require the separate armeabi-v7a process; cross-ISA execution and AOT translation are unavailable"},
    {"swift","Swift","Swift",Status::Unsupported,"No Swift runtime"}
  };
  return rows;
}
std::vector<Capability> neededCapabilities(const Detected& d) {
  std::vector<std::string> ids = {"native_executable","macho_loading","relocations","multi_image_loading","cxx_runtime"};
  auto add = [&](bool yes, const char* id) { if (yes) ids.emplace_back(id); };
  add(d.objc,"objc_runtime"); add(d.objc,"foundation");
  add(d.uikit,"uikit"); add(d.corefoundation,"corefoundation"); add(d.vulkan,"vulkan"); add(d.input,"input"); add(d.gles,"opengles");
  add(d.metal,"metal"); add(d.audio,"audio"); add(d.networking,"networking");
  add(d.swift,"swift"); add(d.arm64e,"arm64e_pac"); add(d.armv7 && !d.hostArm32,"armv7_aot");
  std::vector<Capability> result;
  for (const auto& row : capabilityMatrix()) if (std::find(ids.begin(),ids.end(),row.id)!=ids.end()) result.push_back(row);
  return result;
}
RuntimeState stateFor(const Detected& d) {
  if (!d.blockedReason.empty() || d.encrypted) return RuntimeState::Blocked;
  if (!d.linked) return RuntimeState::Analyzed;
  if (d.unresolvedStrong) return RuntimeState::PartiallyRelinked;
  auto rows = neededCapabilities(d);
  for (const auto& r : rows) if (r.status == Status::Unsupported || r.status == Status::Blocked) return RuntimeState::Relinked;
  for (const auto& r : rows) if (r.status == Status::Partial) return RuntimeState::RuntimePartial;
  return RuntimeState::RuntimeReady;
}
std::string summaryFor(const Detected& d, size_t imports) {
  std::string s = runtimeStateName(stateFor(d));
  s += " - " + std::to_string(imports >= d.unresolvedStrong + d.unresolvedWeak ? imports - d.unresolvedStrong - d.unresolvedWeak : 0) + " of " + std::to_string(imports) + " imports bound; " + std::to_string(d.unresolvedStrong) + " left as named trap stubs";
  if (d.uikit) s += "; unsupported: UIKit (startup shims only; no app UI/event loop)";
  else for (const auto& r : neededCapabilities(d)) if (r.status == Status::Unsupported) { s += "; unsupported: " + r.title; break; }
  if (!d.blockedReason.empty()) s += "; blocked: " + d.blockedReason;
  return s;
}
}
