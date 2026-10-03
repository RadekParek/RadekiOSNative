#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace radeki::ipa {
struct BundleInfo {
  std::string name,bundleIdentifier,executable,executablePath,version,shortVersion,minimumOSVersion,platform,iconPath;
  uint64_t executableSize=0;
  std::vector<std::string> architectures,warnings;
};
bool inspectBundle(const std::string& appPath,const std::string& iconOutPath,BundleInfo& out,std::string& error);
}
