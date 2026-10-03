#pragma once
#include <string>
#include <vector>

#include "mach_o/macho.h"

namespace radeki::analysis {

struct FunctionInfo { uint64_t addr = 0, size = 0; std::string name; };
enum class XrefKind { Branch, Call, CondBranch, AdrpPair, Adr, Literal };
struct Xref { uint64_t from = 0, to = 0; XrefKind kind = XrefKind::Branch; };

struct Report {
  std::vector<FunctionInfo> functions;
  std::vector<Xref> xrefs;
  uint64_t instructions = 0, indirectBranches = 0, returns = 0;
  uint64_t pacSign = 0, pacAuth = 0, pacBranch = 0, pacStrip = 0;
  bool objc = false, swift = false, metal = false, gles = false, audio = false, uikit = false, network = false;
  std::vector<std::string> frameworks;   // dylib install names
  std::vector<std::string> blockers;     // static findings that will block conversion
};

// ARM64/ARM64e code analysis driven by Mach-O metadata (function starts, data-in-code).
Report analyze(const macho::Image& img);

}  // namespace radeki::analysis
