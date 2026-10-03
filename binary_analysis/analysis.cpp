#include "binary_analysis/analysis.h"

#include <algorithm>
#include <map>

#include "arm64/arm64.h"

namespace radeki::analysis {
using namespace macho;

static bool contains(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

Report analyze(const Image& img) {
  Report rep;
  for (const auto& d : img.dylibs) {
    if (d.kind == DylibKind::Id) continue;
    rep.frameworks.push_back(d.name);
    if (contains(d.name, "Metal.framework")) rep.metal = true;
    if (contains(d.name, "OpenGLES")) rep.gles = true;
    if (contains(d.name, "AudioToolbox") || contains(d.name, "AVFoundation") || contains(d.name, "OpenAL")) rep.audio = true;
    if (contains(d.name, "UIKit")) rep.uikit = true;
    if (contains(d.name, "CFNetwork") || contains(d.name, "Network.framework")) rep.network = true;
    if (contains(d.name, "libswift")) rep.swift = true;
    if (contains(d.name, "libobjc")) rep.objc = true;
  }
  for (const auto& s : img.segments)
    for (const auto& x : s.sections) {
      if (x.name.rfind("__objc_", 0) == 0) rep.objc = true;
      if (x.name.rfind("__swift5_", 0) == 0) rep.swift = true;
    }
  if (img.cryptId) rep.blockers.push_back("encrypted_binary: LC_ENCRYPTION_INFO cryptid != 0; encrypted segments are not processed");
  if (img.arch == Arch::ARM64e) rep.blockers.push_back("arm64e: PAC compatibility layer not implemented; auth fixups cannot be linked");
  if (img.arch == Arch::ARMv7 || img.arch == Arch::ARMv7s || img.arch == Arch::ARMv7k || img.arch == Arch::ARMv6) {
#if !defined(__arm__) || defined(__aarch64__)
    rep.blockers.push_back("arm32: guest requires a matching 32-bit ARM process; cross-ISA translation is not implemented");
#endif
  }
  if (img.arch != Arch::ARM64 && img.arch != Arch::ARM64e) return rep;

  std::map<uint64_t, std::string> names;
  for (const auto& s : img.symbols)
    if (!s.undefined() && !s.stab() && !s.name.empty()) names.emplace(s.value, s.name);

  auto inData = [&](uint64_t a) {
    for (auto& d : img.dataInCode) if (a >= d.first && a < d.first + d.second) return true;
    return false;
  };
  Reader rd(img.data);
  uint64_t textEnd = 0;
  for (const auto& seg : img.segments)
    for (const auto& sec : seg.sections) {
      if (!sec.hasCode() || sec.size == 0 || sec.zerofill()) continue;
      textEnd = std::max(textEnd, sec.addr + sec.size);
      for (uint64_t a = sec.addr; a + 4 <= sec.addr + sec.size; a += 4) {
        if (inData(a)) continue;
        uint32_t w = rd.read<uint32_t>(sec.offset + (a - sec.addr), "code");
        a64::Insn i = a64::decode(w);
        ++rep.instructions;
        using K = a64::Kind;
        switch (i.kind) {
          case K::B: rep.xrefs.push_back({a, a64::targetOf(i, a), XrefKind::Branch}); break;
          case K::BL: rep.xrefs.push_back({a, a64::targetOf(i, a), XrefKind::Call}); break;
          case K::BCond: case K::CBZ: case K::TBZ: rep.xrefs.push_back({a, a64::targetOf(i, a), XrefKind::CondBranch}); break;
          case K::ADR: rep.xrefs.push_back({a, a64::targetOf(i, a), XrefKind::Adr}); break;
          case K::LdrLit: rep.xrefs.push_back({a, a64::targetOf(i, a), XrefKind::Literal}); break;
          case K::BR: case K::BLR: ++rep.indirectBranches; break;
          case K::RET: ++rep.returns; break;
          case K::PacSign: ++rep.pacSign; break;
          case K::PacAuth: ++rep.pacAuth; break;
          case K::PacBranch: ++rep.pacBranch; break;
          case K::PacStrip: ++rep.pacStrip; break;
          case K::ADRP: {
            // Heuristic: pair with an immediately following ADD/LDR/STR using the same base register.
            if (a + 8 > sec.addr + sec.size || inData(a + 4)) break;
            uint32_t n = rd.read<uint32_t>(sec.offset + (a + 4 - sec.addr), "code");
            uint64_t page = a64::targetOf(i, a);
            if (((n >> 5) & 31) != i.rd) break;
            if ((n & 0xFF800000) == 0x91000000) {
              uint64_t imm = (n >> 10) & 0xFFF;
              rep.xrefs.push_back({a, page + ((n >> 22 & 1) ? imm << 12 : imm), XrefKind::AdrpPair});
            } else if ((n & 0xFFC00000) == 0xF9400000 || (n & 0xFFC00000) == 0xF9000000) {
              rep.xrefs.push_back({a, page + (((n >> 10) & 0xFFF) << 3), XrefKind::AdrpPair});
            }
            break;
          }
          default: break;
        }
      }
    }

  std::vector<uint64_t> starts = img.functionStarts;
  std::sort(starts.begin(), starts.end());
  for (size_t k = 0; k < starts.size(); ++k) {
    FunctionInfo f;
    f.addr = starts[k];
    f.size = (k + 1 < starts.size() ? starts[k + 1] : textEnd) - starts[k];
    if (k + 1 >= starts.size() && textEnd < starts[k]) f.size = 0;
    auto it = names.find(f.addr);
    if (it != names.end()) f.name = it->second;
    rep.functions.push_back(std::move(f));
  }
  return rep;
}

}  // namespace radeki::analysis
