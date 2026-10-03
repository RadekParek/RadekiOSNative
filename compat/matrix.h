#pragma once
#include "compat/status.h"
#include <string>
#include <vector>
namespace radeki::compat {
struct Capability { std::string id, title, subsystem; Status status; std::string detail; };
struct Detected {
  bool linked = false;
  size_t unresolvedStrong = 0, unresolvedWeak = 0;
  bool objc = false, gles = false, metal = false, audio = false, networking = false;
  bool swift = false, arm64e = false, armv7 = false, hostArm32 = false, encrypted = false;
  bool uikit = false, corefoundation = false, vulkan = false, input = false;
  std::string blockedReason;
};
const std::vector<Capability>& capabilityMatrix();
std::vector<Capability> neededCapabilities(const Detected&);
RuntimeState stateFor(const Detected&);
std::string summaryFor(const Detected&, size_t imports = 0);
}
