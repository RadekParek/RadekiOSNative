#include "relinker/relinker.h"

#include <algorithm>
#include <cstring>

#include "arm64/arm64.h"
#include "core/bytes.h"

namespace radeki::relinker {
using namespace macho;

std::string leafOf(const std::string& p) {
  size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

uint32_t LinkedImage::read32(uint64_t a) const {
  if (!contains(a, 4)) throw LinkError("read32 outside image");
  uint32_t v; std::memcpy(&v, memory.data() + (a - loadBase), 4); return v;
}
uint64_t LinkedImage::read64(uint64_t a) const {
  if (!contains(a, 8)) throw LinkError("read64 outside image");
  uint64_t v; std::memcpy(&v, memory.data() + (a - loadBase), 8); return v;
}
void LinkedImage::write32(uint64_t a, uint32_t v) {
  if (!contains(a, 4)) throw LinkError("write32 outside image");
  std::memcpy(memory.data() + (a - loadBase), &v, 4);
}

LinkedImage link(const Image& img, const LinkOptions& opt) {
  const bool arm32 = isArm32Architecture(img.arch);
  const bool arm64 = img.arch == Arch::ARM64 || img.arch == Arch::ARM64e;
  if (!arm32 && !arm64) throw LinkError(std::string("unsupported architecture for native relink: ") + archName(img.arch));
  if (img.cryptId) throw LinkError("image is encrypted (cryptid != 0); refusing to link encrypted segments");
  if (img.filetype != MH_EXECUTE && img.filetype != MH_DYLIB) throw LinkError("unsupported Mach-O filetype");
  const uint64_t pageSize = pageSizeForArch(img.arch);
  if (opt.loadBase % pageSize) throw LinkError(arm32 ? "ARM32 loadBase must be 4K aligned" : "loadBase must be 16K aligned");
  for (const auto& f : img.fixups)
    if (f.auth) throw LinkError("authenticated pointer fixups require the PAC layer (not implemented)");

  uint64_t minVm = UINT64_MAX, maxVm = 0;
  for (const auto& s : img.segments) {
    if (s.name == "__PAGEZERO" || s.vmsize == 0) continue;
    minVm = std::min(minVm, s.vmaddr);
    maxVm = std::max(maxVm, s.vmaddr + s.vmsize);
  }
  if (minVm == UINT64_MAX) throw LinkError("no mappable segments");
  if (minVm != img.textBase()) throw LinkError("header segment is not the lowest mapped segment");
  if (minVm % pageSize) throw LinkError(arm32 ? "ARM32 image base not 4K aligned" : "image base not 16K aligned");
  uint64_t imageSize = alignUp(maxVm - minVm, pageSize);
  if (imageSize > opt.maxImageBytes) throw LinkError("image too large");
  if (arm32 && (opt.loadBase > UINT32_MAX || imageSize > UINT32_MAX - opt.loadBase))
    throw LinkError("ARM32 image mapping exceeds the 32-bit address space");

  LinkedImage li;
  li.arch = img.arch;
  li.loadBase = opt.loadBase;
  li.imageBase = minVm;
  li.slide = opt.loadBase - minVm;
  if (img.classicBinds && li.slide != 0)
    li.warnings.push_back("classic image (no relocation records): internal pointers are only correct at slide 0; "
                          "link at the preferred vmaddr " + std::to_string(minVm) + " for correct data pointers");
  li.imageSize = imageSize;
  if (opt.loadBase + imageSize + (16ull << 20) < opt.loadBase) throw LinkError("loadBase overflows address space");

  // Resolve imports first so the trap-stub area can be sized.
  struct Res { bool ok; uint64_t target; int trap; bool stubbed; };
  std::vector<Res> res(img.imports.size(), {false, 0, -1, false});
  uint32_t trapCount = 0;
  std::vector<std::string> dylibOf(img.imports.size());
  for (size_t i = 0; i < img.imports.size(); ++i) {
    const Import& im = img.imports[i];
    if (im.libOrdinal >= 1 && size_t(im.libOrdinal) <= img.dylibs.size()) {
      // ordinals count only non-ID dylib commands
      int n = 0;
      for (const auto& d : img.dylibs) if (d.kind != DylibKind::Id && ++n == im.libOrdinal) { dylibOf[i] = d.name; break; }
    }
    std::optional<uint64_t> t;
    if (opt.resolver) t = opt.resolver->resolve(im.name, dylibOf[i]);
    if (t && arm32 && *t > UINT32_MAX) {
      // Host-side shims live above the 32-bit line; on a real ARM32 device they fit.
      // Demote to a trap stub rather than aborting the whole link; still reported unresolved.
      t.reset();
      li.warnings.push_back("import " + im.name + ": resolved target exceeds the ARM32 address"
                            " space (host shim); bound to trap stub");
    }
    if (t) res[i] = {true, *t, -1, false};
    else if (!im.weak) {
      // Unresolved strong import. Default doctrine: a named BRK trap stub that reports itself.
      // With a stub factory (runtime "keep the startup loop alive" mode) the import binds to a
      // host-executable logging stub instead -- still reported unresolved, never faked.
      uint64_t stub = opt.stubFactory ? opt.stubFactory(im.name, dylibOf[i]) : 0;
      if (stub && arm32 && stub > UINT32_MAX) stub = 0;  // host stub address above the 32-bit line: use a trap
      if (stub) res[i] = {false, stub, -1, true};
      else res[i] = {false, 0, int(trapCount++), false};
    }
  }

  uint64_t stubBytes = alignUp(uint64_t(trapCount) * 4, 16) + uint64_t(opt.veneerSlots) * 16;
  li.totalSize = imageSize + alignUp(std::max<uint64_t>(stubBytes, 16), pageSize);
  if (arm32 && li.totalSize > UINT32_MAX - opt.loadBase)
    throw LinkError("ARM32 image and stub mapping exceeds the 32-bit address space");
  li.memory.assign(li.totalSize, 0);

  for (const auto& s : img.segments) {
    if (s.name == "__PAGEZERO" || s.vmsize == 0) continue;
    if (s.filesize) std::memcpy(li.memory.data() + (s.vmaddr - minVm), img.data.data() + s.fileoff, s.filesize);
    li.regions.push_back({s.name, s.vmaddr + li.slide, alignUp(s.vmsize, pageSize), s.initprot, (s.initprot & VM_PROT_EXECUTE) != 0});
    if (s.initprot & VM_PROT_EXECUTE) {
      bool anyCode = false;
      for (const auto& x : s.sections) {
        if (!x.hasCode() || x.zerofill() || x.size == 0) continue;
        anyCode = true;
        li.codeRanges.push_back({x.addr + li.slide, x.addr + li.slide + x.size});
      }
      // A segment that marks no instruction sections at all: treat it as all code, which is
      // what an image without section attributes leaves us to assume.
      if (!anyCode) li.codeRanges.push_back({s.vmaddr + li.slide, s.vmaddr + li.slide + alignUp(s.vmsize, pageSize)});
    }
  }
  uint64_t stubBase = li.loadBase + imageSize;
  li.regions.push_back({"__RADEKI_STUBS", stubBase, li.totalSize - imageSize, VM_PROT_READ | VM_PROT_EXECUTE, true});
  li.codeRanges.push_back({stubBase, stubBase + alignUp(uint64_t(trapCount) * 4, 16)});
  li.veneerBase = stubBase + alignUp(uint64_t(trapCount) * 4, 16);
  li.veneerCap = opt.veneerSlots;

  if (trapCount > 0xFFFF) throw LinkError("too many unresolved imports");
  for (uint32_t t = 0; t < trapCount; ++t) {
    uint64_t addr = stubBase + uint64_t(t) * 4;
    if (arm32) {
      // A32 BKPT #imm16. It raises SIGTRAP in a 32-bit Android process and leaves the
      // unresolved import name in LinkedImage::traps, matching the ARM64 BRK policy.
      uint32_t immediate = t & 0xFFFF;
      li.write32(addr, 0xE1200070u | ((immediate & 0xFFF0u) << 4) | (immediate & 0xFu));
    } else {
      li.write32(addr, 0xD4200000u | (t & 0xFFFF) << 5);  // BRK #t
    }
  }
  for (size_t i = 0; i < img.imports.size(); ++i)
    if (res[i].trap >= 0) {
      uint32_t t = uint32_t(res[i].trap);
      li.traps.push_back({t, stubBase + uint64_t(t) * 4, img.imports[i].name});
    }

  auto slotOff = [&](uint64_t vm, uint64_t n) {
    if (vm < minVm || vm - minVm + n > imageSize) throw LinkError("fixup location outside image");
    return vm - minVm;
  };
  const uint64_t pointerSize = img.ptrSize();
  for (const auto& f : img.fixups) {
    uint64_t o = slotOff(f.addr, pointerSize);
    uint64_t v;
    if (f.kind == Fixup::Kind::Rebase) {
      if (f.target < minVm || f.target > maxVm) li.warnings.push_back("rebase target outside image at " + std::to_string(f.addr));
      v = (f.target + li.slide) | (arm32 ? 0 : (uint64_t(f.high8) << 56));
      ++li.rebases;
    } else {
      const Import& im = img.imports.at(f.importIndex);
      const Res& r = res.at(f.importIndex);
      BoundImport b{im.name, dylibOf[f.importIndex], f.addr + li.slide, r.ok, im.weak, 0, r.trap, compat::SymbolClass::Unsupported, "", "", r.stubbed};
      if (r.ok || r.stubbed) v = r.target + uint64_t(f.addend), b.target = v;
      else if (r.trap >= 0) v = stubBase + uint64_t(r.trap) * 4, b.target = v;
      else v = 0;  // unresolved weak import binds to null, matching dyld semantics
      if (r.ok) {
        auto entry = opt.resolver ? opt.resolver->describe(im.name, dylibOf[f.importIndex]) : std::nullopt;
        if (entry) b.cls = entry->cls, b.framework = entry->framework, b.method = entry->method;
        else b.cls = compat::SymbolClass::GuestImage, b.method = "resolved";
      } else if (r.stubbed) {
        b.cls = compat::SymbolClass::NoopStub;
        b.framework = leafOf(dylibOf[f.importIndex]);
        b.method = "stub_dispatch";
      } else if (im.weak) b.cls = compat::SymbolClass::WeakOptional, b.method = "weak_null";
      else b.cls = compat::SymbolClass::Unsupported, b.method = "trap";
      li.imports.push_back(std::move(b));
      ++li.binds;
    }
    if (arm32) {
      if (v > UINT32_MAX) throw LinkError("ARM32 fixup target exceeds the 32-bit address space");
      uint32_t word = static_cast<uint32_t>(v);
      std::memcpy(li.memory.data() + o, &word, sizeof word);
    } else {
      std::memcpy(li.memory.data() + o, &v, sizeof v);
    }
  }
  for (const auto& s : img.segments)
    for (const auto& x : s.sections)
      if (x.type() == S_MOD_INIT_FUNC_POINTERS)
        for (uint64_t k = 0; k + pointerSize <= x.size; k += pointerSize)
          li.initPointerSlots.push_back(x.addr + k + li.slide);
  for (const auto& sym : img.symbols)
    if (!sym.undefined() && !sym.stab() && sym.value >= minVm && sym.value < maxVm)
      li.definedSymbols.push_back({sym.value + li.slide, sym.name});
  std::sort(li.definedSymbols.begin(), li.definedSymbols.end());
  if (img.entry) li.entry = *img.entry + li.slide;
  for (auto& d : img.dataInCode) li.dataInCode.push_back({d.first + li.slide, d.second});
  return li;
}

void patchBranch(LinkedImage& li, uint64_t site, uint64_t target, bool link) {
  if (isArm32Architecture(li.arch)) throw LinkError("ARM32 branch rewriting is not implemented");
  bool inCode = false;
  for (const auto& r : li.regions) if (r.code && site >= r.addr && site + 4 <= r.addr + r.size) inCode = true;
  if (!inCode || (site & 3)) throw LinkError("branch site is not in executable memory");
  if (auto w = a64::encodeB(site, target, link)) { li.write32(site, *w); return; }
  if (li.veneerCount >= li.veneerCap) throw LinkError("veneer pool exhausted");
  uint64_t v = li.veneerBase + li.veneerCount++ * 16;
  li.codeRanges.push_back({v, v + 16});  // the veneer is code now, so validate() checks it
  li.write32(v, 0x58000050);      // LDR x16, #8
  li.write32(v + 4, 0xD61F0200);  // BR x16
  uint64_t o = v - li.loadBase + 8;
  std::memcpy(li.memory.data() + o, &target, 8);
  auto w = a64::encodeB(site, v, link);
  if (!w) throw LinkError("veneer out of branch range");
  li.write32(site, *w);
}

Validation validate(const LinkedImage& li) {
  Validation out;
  if (isArm32Architecture(li.arch)) {
    out.warnings.push_back({li.loadBase, "ARMv7 PC-relative instruction validation is not implemented"});
    return out;
  }
  auto inData = [&](uint64_t a) {
    for (auto& d : li.dataInCode) if (a >= d.first && a < d.first + d.second) return true;
    return false;
  };
  std::vector<std::pair<uint64_t, uint64_t>> ranges = li.codeRanges;
  if (ranges.empty())
    for (const auto& r : li.regions)
      if (r.code) ranges.push_back({r.addr, r.addr + r.size});
  for (const auto& range : ranges) {
    uint64_t start = range.first, end = range.second;
    for (uint64_t a = start; a + 4 <= end; a += 4) {
      if (inData(a)) continue;
      a64::Insn i = a64::decode(li.read32(a));
      if (!i.pcRel()) continue;
      ++out.checked;
      uint64_t t = a64::targetOf(i, a);
      if (li.contains(t)) continue;
      Violation v{a, "pc-relative reference to 0x" + std::to_string(t) + " leaves the image"};
      if (i.kind == a64::Kind::ADRP) out.warnings.push_back(v); else out.violations.push_back(v);
    }
  }
  return out;
}

}  // namespace radeki::relinker

namespace radeki::relinker {
std::optional<std::string> LinkedImage::symbolAt(uint64_t pc) const {
  auto it = std::upper_bound(definedSymbols.begin(), definedSymbols.end(), pc,
    [](uint64_t a, const auto& b) { return a < b.first; });
  if (it == definedSymbols.begin()) return std::nullopt;
  --it; if (pc - it->first >= 65536) return std::nullopt; return it->second;
}
std::optional<std::string> LinkedImage::importAtTrap(uint64_t addr) const {
  for (const auto& t : traps) if (t.addr == addr) return t.symbol;
  return std::nullopt;
}
}
