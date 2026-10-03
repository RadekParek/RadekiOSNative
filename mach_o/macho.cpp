#include "mach_o/macho.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace radeki::macho {
namespace {

constexpr uint32_t MH_MAGIC = 0xFEEDFACE, MH_MAGIC_64 = 0xFEEDFACF;
constexpr uint32_t MH_CIGAM = 0xCEFAEDFE, MH_CIGAM_64 = 0xCFFAEDFE;
constexpr uint32_t FAT_MAGIC = 0xCAFEBABE, FAT_MAGIC_64 = 0xCAFEBABF;

constexpr uint32_t LC_REQ_DYLD = 0x80000000u;
enum : uint32_t {
  LC_SEGMENT = 0x1, LC_SYMTAB = 0x2, LC_UNIXTHREAD = 0x5, LC_DYSYMTAB = 0xB, LC_LOAD_DYLIB = 0xC,
  LC_ID_DYLIB = 0xD, LC_LAZY_LOAD_DYLIB = 0x20, LC_ENCRYPTION_INFO = 0x21, LC_DYLD_INFO = 0x22,
  LC_SEGMENT_64 = 0x19, LC_UUID = 0x1B, LC_CODE_SIGNATURE = 0x1D, LC_FUNCTION_STARTS = 0x26,
  LC_DATA_IN_CODE = 0x29, LC_ENCRYPTION_INFO_64 = 0x2C, LC_VERSION_MIN_IPHONEOS = 0x25,
  LC_BUILD_VERSION = 0x32,
  LC_LOAD_WEAK_DYLIB = 0x18 | LC_REQ_DYLD, LC_RPATH = 0x1C | LC_REQ_DYLD,
  LC_REEXPORT_DYLIB = 0x1F | LC_REQ_DYLD, LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD,
  LC_LOAD_UPWARD_DYLIB = 0x23 | LC_REQ_DYLD, LC_MAIN = 0x28 | LC_REQ_DYLD,
  LC_DYLD_EXPORTS_TRIE = 0x33 | LC_REQ_DYLD, LC_DYLD_CHAINED_FIXUPS = 0x34 | LC_REQ_DYLD,
};

constexpr size_t kMaxLoadCommands = 4096;
constexpr uint64_t kMaxSymbols = 5'000'000;
constexpr size_t kMaxFixups = 20'000'000;

struct Pending {
  uint32_t dyldInfo[10] = {};
  bool haveDyldInfo = false;
  uint32_t chainedOff = 0, chainedSize = 0;
  bool haveChained = false;
  uint32_t fnStartsOff = 0, fnStartsSize = 0;
  bool haveFnStarts = false;
  uint32_t dicOff = 0, dicSize = 0;
  bool haveDic = false;
  uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
  bool haveSymtab = false;
  uint32_t indirectOff = 0, nindirect = 0;
  bool haveDysymtab = false;
  std::optional<uint64_t> mainEntryOff;
  std::optional<uint64_t> threadPc;
};

std::string formatUuid(const uint8_t* p) {
  char b[40];
  snprintf(b, sizeof b, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", p[0], p[1], p[2],
           p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
  return b;
}

class ImportTable {
 public:
  explicit ImportTable(Image& img) : img_(img) {}
  uint32_t get(const std::string& name, int ordinal, bool weak) {
    std::string key = name + '\0' + std::to_string(ordinal) + (weak ? 'w' : 's');
    auto it = idx_.find(key);
    if (it != idx_.end()) return it->second;
    uint32_t i = static_cast<uint32_t>(img_.imports.size());
    img_.imports.push_back({name, ordinal, weak});
    idx_[key] = i;
    return i;
  }

 private:
  Image& img_;
  std::map<std::string, uint32_t> idx_;
};

void checkFixupBudget(const Image& img) {
  if (img.fixups.size() >= kMaxFixups) throw FormatError("too many fixups");
}

uint64_t readPtr(const Image& img, uint64_t addr) {
  auto fo = img.vmToFile(addr, img.ptrSize());
  if (!fo) throw FormatError("fixup location is not file-backed");
  Reader r(img.data);
  return img.is64 ? r.read<uint64_t>(*fo, "pointer") : r.read<uint32_t>(*fo, "pointer");
}

const Segment& segAt(const Image& img, int64_t idx) {
  if (idx < 0 || static_cast<uint64_t>(idx) >= img.segments.size()) throw FormatError("fixup segment index out of range");
  return img.segments[idx];
}

uint64_t fixupAddr(const Image& img, int64_t segIdx, uint64_t segOff) {
  const Segment& s = segAt(img, segIdx);
  uint64_t end;
  if (__builtin_add_overflow(segOff, img.ptrSize(), &end) || end > s.vmsize) throw FormatError("fixup offset outside segment");
  return s.vmaddr + segOff;
}

void parseRebaseOpcodes(Image& img, const Reader& r, uint32_t off, uint32_t size) {
  r.need(off, size, "rebase info");
  uint64_t p = off, end = uint64_t(off) + size;
  uint8_t type = 1;
  int64_t segIdx = -1;
  uint64_t segOff = 0;
  const uint64_t ps = img.ptrSize();
  auto doRebase = [&] {
    if (type != 1) throw FormatError("unsupported rebase type");
    checkFixupBudget(img);
    Fixup f;
    f.kind = Fixup::Kind::Rebase;
    f.addr = fixupAddr(img, segIdx, segOff);
    f.target = readPtr(img, f.addr);
    img.fixups.push_back(f);
    segOff += ps;
  };
  while (p < end) {
    uint8_t b = r.read<uint8_t>(p++, "rebase opcode");
    uint8_t imm = b & 0x0F;
    switch (b & 0xF0) {
      case 0x00: p = end; break;
      case 0x10: type = imm; break;
      case 0x20: segIdx = imm; segOff = r.uleb(p, end, "rebase offset"); break;
      case 0x30: segOff += r.uleb(p, end, "rebase add"); break;
      case 0x40: segOff += uint64_t(imm) * ps; break;
      case 0x50: for (uint8_t i = 0; i < imm; ++i) doRebase(); break;
      case 0x60: { uint64_t n = r.uleb(p, end, "rebase count"); for (uint64_t i = 0; i < n; ++i) doRebase(); break; }
      case 0x70: doRebase(); segOff += r.uleb(p, end, "rebase add"); break;
      case 0x80: {
        uint64_t n = r.uleb(p, end, "rebase count");
        uint64_t skip = r.uleb(p, end, "rebase skip");
        for (uint64_t i = 0; i < n; ++i) { doRebase(); segOff += skip; }
        break;
      }
      default: throw FormatError("unknown rebase opcode");
    }
  }
}

void parseBindOpcodes(Image& img, ImportTable& imports, const Reader& r, uint32_t off, uint32_t size, bool lazy) {
  r.need(off, size, "bind info");
  uint64_t p = off, end = uint64_t(off) + size;
  int ordinal = 0;
  std::string symbol;
  uint8_t flags = 0, type = 1;
  int64_t addend = 0, segIdx = -1;
  uint64_t segOff = 0;
  const uint64_t ps = img.ptrSize();
  struct ThreadedBind { uint32_t importIndex; int64_t addend; };
  std::vector<ThreadedBind> threadedTable;
  bool threadedMode = false;
  auto doBind = [&] {
    if (type != 1) throw FormatError("unsupported bind type");
    if (symbol.empty()) throw FormatError("bind without symbol");
    uint32_t idx = imports.get(symbol, ordinal, (flags & 1) != 0);
    if (threadedMode) {
      if (threadedTable.size() >= 1'000'000) throw FormatError("threaded bind table too large");
      threadedTable.push_back({idx, addend});
      return;
    }
    checkFixupBudget(img);
    Fixup f;
    f.kind = Fixup::Kind::Bind;
    f.addr = fixupAddr(img, segIdx, segOff);
    f.importIndex = idx;
    f.addend = addend;
    f.lazy = lazy;
    img.fixups.push_back(f);
  };
  while (p < end) {
    uint8_t b = r.read<uint8_t>(p++, "bind opcode");
    uint8_t imm = b & 0x0F;
    switch (b & 0xF0) {
      case 0x00: if (!lazy) p = end; break;
      case 0x10: ordinal = imm; break;
      case 0x20: { uint64_t v = r.uleb(p, end, "bind ordinal"); if (v > 0xFFFF) throw FormatError("bind ordinal too large"); ordinal = int(v); break; }
      case 0x30: ordinal = imm == 0 ? 0 : int(int8_t(0xF0 | imm)); break;
      case 0x40: flags = imm; symbol = r.cstr(p, end, "bind symbol"); p += symbol.size() + 1; break;
      case 0x50: type = imm; break;
      case 0x60: addend = r.sleb(p, end, "bind addend"); break;
      case 0x70: segIdx = imm; segOff = r.uleb(p, end, "bind offset"); break;
      case 0x80: segOff += r.uleb(p, end, "bind add"); break;
      case 0x90: doBind(); if (!threadedMode) segOff += ps; break;
      case 0xA0: doBind(); segOff += ps + r.uleb(p, end, "bind add"); break;
      case 0xB0: doBind(); segOff += ps + uint64_t(imm) * ps; break;
      case 0xC0: {
        uint64_t n = r.uleb(p, end, "bind count");
        uint64_t skip = r.uleb(p, end, "bind skip");
        for (uint64_t i = 0; i < n; ++i) { doBind(); segOff += ps + skip; }
        break;
      }
      case 0xD0: {
        if (imm == 0x00) {
          uint64_t n = r.uleb(p, end, "threaded bind table size");
          if (n > 1'000'000) throw FormatError("threaded bind table too large");
          threadedTable.clear();
          threadedTable.reserve(size_t(n));
          threadedMode = true;
        } else if (imm == 0x01) {
          uint64_t curOff = segOff;
          for (size_t step = 0;; ++step) {
            if (step > 1'000'000) throw FormatError("threaded chain too long");
            checkFixupBudget(img);
            uint64_t addr = fixupAddr(img, segIdx, curOff);
            uint64_t raw = readPtr(img, addr);
            Fixup f;
            f.addr = addr;
            if ((raw >> 62) & 1) {
              uint32_t ord = uint32_t(raw & 0xFFFF);
              if (ord >= threadedTable.size()) throw FormatError("threaded bind ordinal out of range");
              f.kind = Fixup::Kind::Bind;
              f.importIndex = threadedTable[ord].importIndex;
              f.addend = threadedTable[ord].addend;
            } else {
              f.kind = Fixup::Kind::Rebase;
              f.target = raw & 0x7FFFFFFFFFFull;
              f.high8 = uint8_t((raw >> 43) & 0xFF);
            }
            img.fixups.push_back(f);
            uint64_t delta = (raw >> 51) & 0x7FF;
            if (!delta) break;
            curOff += delta * 8;
          }
        } else {
          throw FormatError("unsupported threaded bind subopcode");
        }
        break;
      }
      default: throw FormatError("unsupported bind opcode");
    }
  }
}
// Classic (pre-dyld-info) binding. Older images carry no rebase/bind opcodes: dyld 1 loaded
// them at their preferred vmaddr (slide 0) and the only external fixups are the
// __nl_symbol_ptr / __la_symbol_ptr slots, each resolving through the section's range of the
// indirect symbol table. __symbol_stub code reads those same lazy slots, so stub calls are
// covered by binding the slots alone.
void synthesizeClassicBinds(Image& img, ImportTable& imports) {
  img.classicBinds = true;
  const uint64_t ps = img.ptrSize();
  std::map<uint32_t, uint32_t> importForSymbol;  // symtab index -> import index
  for (const auto& s : img.segments)
    for (const auto& x : s.sections) {
      const uint32_t t = x.type();
      if (t != S_NON_LAZY_SYMBOL_POINTERS && t != S_LAZY_SYMBOL_POINTERS) continue;
      if (x.size % ps != 0) throw FormatError("symbol pointer section size is not pointer-aligned");
      const uint64_t n = x.size / ps;
      if (x.reserved1 > img.indirectSymbols.size() ||
          n > img.indirectSymbols.size() - x.reserved1)
        throw FormatError("symbol pointer section exceeds indirect symbol table");
      for (uint64_t i = 0; i < n; ++i) {
        const size_t e = size_t(x.reserved1) + size_t(i);
        const uint32_t symIdx = img.indirectSymbols[e];
        if (symIdx & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;  // absolute/local markers
        if (symIdx >= img.symbols.size()) throw FormatError("indirect symbol index outside symbol table");
        const Symbol& sym = img.symbols[symIdx];
        if (sym.stab() || !sym.external() || !sym.undefined()) continue;  // bound to self image
        uint32_t imp;
        auto it = importForSymbol.find(symIdx);
        if (it == importForSymbol.end()) {
          // The library ordinal occupies the high byte of n_desc; values 0xFF..0xFC
          // encode the signed special ordinals (-1..-4), just like dyld bind opcodes.
          const uint8_t raw = static_cast<uint8_t>(sym.libOrdinal());
          const int ordinal = raw < 0x80 ? static_cast<int>(raw) : static_cast<int>(raw) - 0x100;
          const bool weak = (sym.desc & 0x0040) != 0;  // N_WEAK_REF
          imp = imports.get(sym.name, ordinal, weak);
          importForSymbol.emplace(symIdx, imp);
        } else imp = it->second;
        checkFixupBudget(img);
        Fixup f;
        f.kind = Fixup::Kind::Bind;
        f.addr = x.addr + i * ps;
        f.importIndex = imp;
        f.lazy = t == S_LAZY_SYMBOL_POINTERS;
        img.fixups.push_back(f);
      }
    }
}

void parseChainedFixups(Image& img, ImportTable& table, const Reader& r, uint32_t off, uint32_t size) {
  Reader c = r.sub(off, size, "chained fixups");
  if (c.read<uint32_t>(0, "fixups_version") != 0) throw FormatError("unsupported chained fixups version");
  uint32_t startsOff = c.read<uint32_t>(4), importsOff = c.read<uint32_t>(8), symbolsOff = c.read<uint32_t>(12);
  uint32_t importsCount = c.read<uint32_t>(16), importsFormat = c.read<uint32_t>(20), symbolsFormat = c.read<uint32_t>(24);
  if (symbolsFormat != 0) throw FormatError("compressed chained-fixup symbol pool unsupported");
  if (importsCount > 1'000'000) throw FormatError("too many chained imports");
  uint32_t entSize = importsFormat == 1 ? 4 : importsFormat == 2 ? 8 : importsFormat == 3 ? 16 : 0;
  if (!entSize) throw FormatError("unknown chained import format");
  c.need(importsOff, uint64_t(importsCount) * entSize, "chained imports");

  std::vector<uint32_t> importIdx(importsCount);
  std::vector<int64_t> importAddend(importsCount, 0);
  for (uint32_t i = 0; i < importsCount; ++i) {
    uint64_t o = uint64_t(importsOff) + uint64_t(i) * entSize;
    int ordinal; bool weak; uint64_t nameOff; int64_t addend = 0;
    if (importsFormat == 3) {
      uint64_t v = c.read<uint64_t>(o);
      int64_t lo = int16_t(v & 0xFFFF);
      ordinal = (v & 0xFFFF) > 0xFFF0 ? int(lo) : int(v & 0xFFFF);
      weak = (v >> 16) & 1;
      nameOff = v >> 32;
      addend = c.read<int64_t>(o + 8);
    } else {
      uint32_t v = c.read<uint32_t>(o);
      uint32_t lo = v & 0xFF;
      ordinal = lo > 0xF0 ? int(int8_t(lo)) : int(lo);
      weak = (v >> 8) & 1;
      nameOff = v >> 9;
      if (importsFormat == 2) addend = c.read<int32_t>(o + 4);
    }
    uint64_t so;
    if (__builtin_add_overflow(uint64_t(symbolsOff), nameOff, &so)) throw FormatError("chained import name offset overflow");
    importIdx[i] = table.get(c.cstr(so, size, "chained import name"), ordinal, weak);
    importAddend[i] = addend;
  }

  uint32_t segCount = c.read<uint32_t>(startsOff, "starts_in_image");
  if (segCount > 1024) throw FormatError("too many chained segments");
  const uint64_t textBase = img.textBase();
  for (uint32_t s = 0; s < segCount; ++s) {
    uint32_t segInfoOff = c.read<uint32_t>(uint64_t(startsOff) + 4 + uint64_t(s) * 4, "seg_info_offset");
    if (!segInfoOff) continue;
    const Segment& seg = segAt(img, s);
    uint64_t b = uint64_t(startsOff) + segInfoOff;
    uint16_t pageSize = c.read<uint16_t>(b + 4), fmt = c.read<uint16_t>(b + 6);
    uint64_t segmentOffset = c.read<uint64_t>(b + 8);
    uint16_t pageCount = c.read<uint16_t>(b + 20);
    if (pageSize < 0x1000 || (pageSize & (pageSize - 1))) throw FormatError("bad chained page size");
    if (segmentOffset != seg.vmaddr - textBase) throw FormatError("chained segment offset mismatch");
    c.need(b + 22, uint64_t(pageCount) * 2, "page_start");
    img.chainedPointerFormat = fmt;
    const bool arm64eFmt = fmt == 1 || fmt == 9 || fmt == 12;
    if (!(arm64eFmt || fmt == 2 || fmt == 6)) throw FormatError("unsupported chained pointer format " + std::to_string(fmt));
    const uint64_t stride = arm64eFmt ? 8 : 4;

    auto walk = [&](uint64_t loc) {
      for (;;) {
        checkFixupBudget(img);
        auto fo = img.vmToFile(loc, 8);
        if (!fo) throw FormatError("chained fixup outside file-backed segment data");
        uint64_t raw = Reader(img.data).read<uint64_t>(*fo);
        Fixup f;
        f.addr = loc;
        uint64_t next;
        if (!arm64eFmt) {
          next = (raw >> 51) & 0xFFF;
          if (raw >> 63) {
            uint32_t ord = raw & 0xFFFFFF;
            if (ord >= importsCount) throw FormatError("chained bind ordinal out of range");
            f.kind = Fixup::Kind::Bind;
            f.importIndex = importIdx[ord];
            f.addend = int64_t((raw >> 24) & 0xFF) + importAddend[ord];
          } else {
            f.kind = Fixup::Kind::Rebase;
            f.target = (raw & 0xFFFFFFFFFull) + (fmt == 6 ? textBase : 0);
            f.high8 = (raw >> 36) & 0xFF;
          }
        } else {
          next = (raw >> 51) & 0x7FF;
          bool auth = raw >> 63, bind = (raw >> 62) & 1;
          f.auth = auth;
          if (auth) {
            f.diversity = (raw >> 32) & 0xFFFF;
            f.addrDiv = (raw >> 48) & 1;
            f.key = (raw >> 49) & 3;
          }
          if (bind) {
            uint32_t ord = fmt == 12 ? (raw & 0xFFFFFF) : (raw & 0xFFFF);
            if (ord >= importsCount) throw FormatError("chained bind ordinal out of range");
            f.kind = Fixup::Kind::Bind;
            f.importIndex = importIdx[ord];
            int64_t add = 0;
            if (!auth) { add = int64_t((raw >> 32) & 0x7FFFF); if (add & 0x40000) add -= 0x80000; }
            f.addend = add + importAddend[ord];
          } else {
            f.kind = Fixup::Kind::Rebase;
            if (auth) f.target = textBase + (raw & 0xFFFFFFFFull);
            else {
              f.target = (raw & 0x7FFFFFFFFFFull) + (fmt == 1 ? 0 : textBase);
              f.high8 = (raw >> 43) & 0xFF;
            }
          }
        }
        img.fixups.push_back(f);
        if (!next) break;
        loc += next * stride;
      }
    };

    for (uint32_t pg = 0; pg < pageCount; ++pg) {
      uint16_t ps = c.read<uint16_t>(b + 22 + uint64_t(pg) * 2);
      if (ps == 0xFFFF) continue;
      uint64_t pageBase = seg.vmaddr + uint64_t(pg) * pageSize;
      if (ps & 0x8000) {
        uint32_t k = ps & 0x7FFF;
        for (;;) {
          if (k >= pageCount) throw FormatError("chained overflow index out of range");
          uint16_t e = c.read<uint16_t>(b + 22 + uint64_t(k) * 2);
          walk(pageBase + (e & 0x7FFF));
          if (e & 0x8000) break;
          ++k;
        }
      } else {
        walk(pageBase + ps);
      }
    }
  }
  img.hasChainedFixups = true;
}

}  // namespace

const char* archName(Arch a) {
  switch (a) {
    case Arch::ARMv6: return "armv6";
    case Arch::ARMv7: return "armv7";
    case Arch::ARMv7s: return "armv7s";
    case Arch::ARMv7k: return "armv7k";
    case Arch::ARM64: return "arm64";
    case Arch::ARM64e: return "arm64e";
    case Arch::ARM64_32: return "arm64_32";
    case Arch::X86_64: return "x86_64";
    case Arch::I386: return "i386";
    default: return "unknown";
  }
}

Arch archFromCpu(uint32_t cputype, uint32_t cpusubtype) {
  uint32_t sub = cpusubtype & 0x00FFFFFF;
  switch (cputype) {
    case 12:
      return sub == 6 ? Arch::ARMv6 : sub == 9 ? Arch::ARMv7 : sub == 11 ? Arch::ARMv7s : sub == 12 ? Arch::ARMv7k : Arch::Unknown;
    case 0x0100000C: return sub == 2 ? Arch::ARM64e : Arch::ARM64;
    case 0x0200000C: return Arch::ARM64_32;
    case 0x01000007: return Arch::X86_64;
    case 7: return Arch::I386;
    default: return Arch::Unknown;
  }
}

uint64_t Image::textBase() const {
  for (const auto& s : segments)
    if (s.fileoff == 0 && s.filesize > 0) return s.vmaddr;
  for (const auto& s : segments)
    if (s.name != "__PAGEZERO" && s.vmsize > 0) return s.vmaddr;
  throw FormatError("image has no mappable segment");
}

const Segment* Image::findSegment(const std::string& n) const {
  for (const auto& s : segments) if (s.name == n) return &s;
  return nullptr;
}

const Section* Image::findSection(const std::string& seg, const std::string& sect) const {
  for (const auto& s : segments)
    for (const auto& x : s.sections)
      if (x.segment == seg && x.name == sect) return &x;
  return nullptr;
}

bool Image::hasSectionNamed(const std::string& sect) const {
  for (const auto& s : segments)
    for (const auto& x : s.sections) if (x.name == sect) return true;
  return false;
}

std::optional<uint64_t> Image::vmToFile(uint64_t vmaddr, uint64_t len) const {
  for (const auto& s : segments) {
    if (s.filesize == 0 || vmaddr < s.vmaddr) continue;
    uint64_t rel = vmaddr - s.vmaddr, relEnd;
    if (__builtin_add_overflow(rel, len, &relEnd) || relEnd > s.filesize) continue;
    return s.fileoff + rel;
  }
  return std::nullopt;
}

std::vector<SliceInfo> listSlices(Bytes file) {
  Reader r(file);
  if (file.size() < 8) throw FormatError("file too small to be a Mach-O");
  uint32_t be = r.be32(0);
  std::vector<SliceInfo> out;
  if (be == FAT_MAGIC || be == FAT_MAGIC_64) {
    bool wide = be == FAT_MAGIC_64;
    uint32_t n = r.be32(4);
    if (n == 0 || n > 64) throw FormatError("implausible fat architecture count");
    const uint32_t ent = wide ? 32 : 20;
    r.need(8, uint64_t(n) * ent, "fat arch table");
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t o = 8 + uint64_t(i) * ent;
      SliceInfo s;
      s.fat = true;
      s.cputype = r.be32(o);
      s.cpusubtype = r.be32(o + 4);
      s.offset = wide ? r.be64(o + 8) : r.be32(o + 8);
      s.size = wide ? r.be64(o + 16) : r.be32(o + 12);
      if (!r.inRange(s.offset, s.size) || s.size < 4) throw FormatError("fat slice outside file");
      s.arch = archFromCpu(s.cputype, s.cpusubtype);
      out.push_back(s);
    }
    return out;
  }
  uint32_t magic = r.read<uint32_t>(0);
  SliceInfo s;
  s.offset = 0;
  s.size = file.size();
  if (magic == MH_MAGIC || magic == MH_MAGIC_64) {
    s.cputype = r.read<uint32_t>(4);
    s.cpusubtype = r.read<uint32_t>(8);
  } else if (magic == MH_CIGAM || magic == MH_CIGAM_64) {
    s.bigEndian = true;
    s.cputype = r.be32(4);
    s.cpusubtype = r.be32(8);
  } else {
    throw FormatError("not a Mach-O file (bad magic)");
  }
  s.arch = archFromCpu(s.cputype, s.cpusubtype);
  out.push_back(s);
  return out;
}

SliceChoice chooseSlice(const std::vector<SliceInfo>& slices) {
  static const Arch order[] = {Arch::ARM64, Arch::ARM64e, Arch::ARMv7, Arch::ARMv7s, Arch::ARMv7k, Arch::ARMv6};
  for (Arch want : order)
    for (size_t i = 0; i < slices.size(); ++i)
      if (slices[i].arch == want && !slices[i].bigEndian) return {i, std::string("selected ") + archName(want)};
  std::string why = "no supported slice; found:";
  for (const auto& s : slices) {
    why += ' ';
    why += archName(s.arch);
    why += s.arch == Arch::Unknown ? "(cputype " + std::to_string(s.cputype) + ")" : "";
    why += s.bigEndian ? "(big-endian)" : "";
  }
  return {std::nullopt, why};
}

SliceChoice chooseSliceForHost(const std::vector<SliceInfo>& slices) {
#if defined(__arm__) && !defined(__aarch64__)
  static const Arch order[] = {Arch::ARMv7, Arch::ARMv7s, Arch::ARMv7k, Arch::ARMv6};
  constexpr const char* host = "AArch32 process";
#elif defined(__aarch64__)
  static const Arch order[] = {Arch::ARM64, Arch::ARM64e};
  constexpr const char* host = "AArch64 process";
#else
  return chooseSlice(slices);
#endif
#if defined(__arm__) && !defined(__aarch64__) || defined(__aarch64__)
  for (Arch want : order)
    for (size_t i = 0; i < slices.size(); ++i)
      if (slices[i].arch == want && !slices[i].bigEndian)
        return {i, std::string("selected ") + archName(want) + " for the host process"};
  std::string why = std::string("no native guest slice can run in this ") + host + "; found:";
  for (const auto& s : slices) {
    why += ' ';
    why += archName(s.arch);
    why += s.arch == Arch::Unknown ? "(cputype " + std::to_string(s.cputype) + ")" : "";
    why += s.bigEndian ? "(big-endian)" : "";
  }
  return {std::nullopt, why};
#endif
}

Image parseSlice(Bytes file, const SliceInfo& si) {
  Reader fr(file);
  Reader r = fr.sub(si.offset, si.size, "slice");
  uint32_t magic = r.read<uint32_t>(0, "magic");
  Image img;
  if (magic == MH_MAGIC_64) img.is64 = true;
  else if (magic == MH_MAGIC) img.is64 = false;
  else if (magic == MH_CIGAM || magic == MH_CIGAM_64) throw FormatError("big-endian Mach-O is not supported");
  else throw FormatError("bad Mach-O magic");
  const uint32_t hdr = img.is64 ? 32 : 28;
  r.need(0, hdr, "mach header");
  img.data = r.bytes();
  img.cputype = r.read<uint32_t>(4);
  img.cpusubtype = r.read<uint32_t>(8);
  img.filetype = r.read<uint32_t>(12);
  uint32_t ncmds = r.read<uint32_t>(16), sizeofcmds = r.read<uint32_t>(20);
  img.flags = r.read<uint32_t>(24);
  img.arch = archFromCpu(img.cputype, img.cpusubtype);
  bool cpu64 = (img.cputype & 0x01000000) != 0;
  if (cpu64 != img.is64) throw FormatError("cputype/magic width mismatch");
  if (ncmds > kMaxLoadCommands) throw FormatError("too many load commands");
  r.need(hdr, sizeofcmds, "load commands");
  if (uint64_t(ncmds) * 8 > sizeofcmds) throw FormatError("load command table too small");

  Pending pend;
  uint64_t off = hdr;
  const uint64_t cmdEnd = uint64_t(hdr) + sizeofcmds;
  ImportTable importTable(img);
  for (uint32_t i = 0; i < ncmds; ++i) {
    if (off + 8 > cmdEnd) throw FormatError("load command header outside table");
    uint32_t cmd = r.read<uint32_t>(off), cmdsize = r.read<uint32_t>(off + 4);
    if (cmdsize < 8 || (cmdsize & 3) || off + cmdsize > cmdEnd) throw FormatError("bad load command size");
    Reader c = r.sub(off, cmdsize, "load command");
    switch (cmd) {
      case LC_SEGMENT:
      case LC_SEGMENT_64: {
        bool seg64 = cmd == LC_SEGMENT_64;
        if (seg64 != img.is64) throw FormatError("segment command width does not match image");
        const uint32_t base = seg64 ? 72 : 56, sectSize = seg64 ? 80 : 68;
        if (cmdsize < base) throw FormatError("segment command too small");
        Segment s;
        s.name = c.fixedName(8, 16, "segment name");
        uint32_t nsects;
        if (seg64) {
          s.vmaddr = c.read<uint64_t>(24); s.vmsize = c.read<uint64_t>(32);
          s.fileoff = c.read<uint64_t>(40); s.filesize = c.read<uint64_t>(48);
          s.maxprot = c.read<uint32_t>(56); s.initprot = c.read<uint32_t>(60);
          nsects = c.read<uint32_t>(64); s.flags = c.read<uint32_t>(68);
        } else {
          s.vmaddr = c.read<uint32_t>(24); s.vmsize = c.read<uint32_t>(28);
          s.fileoff = c.read<uint32_t>(32); s.filesize = c.read<uint32_t>(36);
          s.maxprot = c.read<uint32_t>(40); s.initprot = c.read<uint32_t>(44);
          nsects = c.read<uint32_t>(48); s.flags = c.read<uint32_t>(52);
        }
        if (uint64_t(nsects) * sectSize + base > cmdsize) throw FormatError("segment section table exceeds command");
        uint64_t vmEnd;
        if (__builtin_add_overflow(s.vmaddr, s.vmsize, &vmEnd)) throw FormatError("segment vm range overflows");
        if (s.filesize > s.vmsize) throw FormatError("segment filesize exceeds vmsize");
        if (s.filesize && !r.inRange(s.fileoff, s.filesize)) throw FormatError("segment file range outside slice");
        for (uint32_t k = 0; k < nsects; ++k) {
          uint64_t so = base + uint64_t(k) * sectSize;
          Section x;
          x.name = c.fixedName(so, 16, "section name");
          x.segment = c.fixedName(so + 16, 16, "section segment");
          if (seg64) {
            x.addr = c.read<uint64_t>(so + 32); x.size = c.read<uint64_t>(so + 40);
            x.offset = c.read<uint32_t>(so + 48); x.align = c.read<uint32_t>(so + 52);
            x.reloff = c.read<uint32_t>(so + 56); x.nreloc = c.read<uint32_t>(so + 60);
            x.flags = c.read<uint32_t>(so + 64); x.reserved1 = c.read<uint32_t>(so + 68);
            x.reserved2 = c.read<uint32_t>(so + 72);
          } else {
            x.addr = c.read<uint32_t>(so + 32); x.size = c.read<uint32_t>(so + 36);
            x.offset = c.read<uint32_t>(so + 40); x.align = c.read<uint32_t>(so + 44);
            x.reloff = c.read<uint32_t>(so + 48); x.nreloc = c.read<uint32_t>(so + 52);
            x.flags = c.read<uint32_t>(so + 56); x.reserved1 = c.read<uint32_t>(so + 60);
            x.reserved2 = c.read<uint32_t>(so + 64);
          }
          uint64_t secEnd;
          if (__builtin_add_overflow(x.addr, x.size, &secEnd) || x.addr < s.vmaddr || secEnd > vmEnd)
            throw FormatError("section outside its segment");
          if (!x.zerofill() && x.size && !r.inRange(x.offset, x.size)) throw FormatError("section file range outside slice");
          if (x.type() == S_THREAD_LOCAL_VARIABLES || x.type() == S_THREAD_LOCAL_REGULAR || x.type() == S_THREAD_LOCAL_ZEROFILL) img.hasTLS = true;
          if (x.type() == S_MOD_INIT_FUNC_POINTERS) ++img.initFuncSections;
          if (x.name == "__unwind_info") img.hasUnwindInfo = true;
          if (x.name == "__eh_frame") img.hasEhFrame = true;
          s.sections.push_back(std::move(x));
        }
        img.segments.push_back(std::move(s));
        break;
      }
      case LC_SYMTAB:
        pend.symoff = c.read<uint32_t>(8); pend.nsyms = c.read<uint32_t>(12);
        pend.stroff = c.read<uint32_t>(16); pend.strsize = c.read<uint32_t>(20);
        pend.haveSymtab = true;
        break;
      case LC_DYSYMTAB:
        pend.indirectOff = c.read<uint32_t>(56); pend.nindirect = c.read<uint32_t>(60);
        pend.haveDysymtab = true;
        break;
      case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LAZY_LOAD_DYLIB:
      case LC_LOAD_UPWARD_DYLIB: case LC_ID_DYLIB: {
        if (cmdsize < 24) throw FormatError("dylib command too small");
        uint32_t no = c.read<uint32_t>(8);
        if (no < 24 || no >= cmdsize) throw FormatError("bad dylib name offset");
        Dylib d;
        d.name = c.cstr(no, cmdsize, "dylib name");
        d.currentVersion = c.read<uint32_t>(16);
        d.compatVersion = c.read<uint32_t>(20);
        d.kind = cmd == LC_LOAD_DYLIB ? DylibKind::Load : cmd == LC_LOAD_WEAK_DYLIB ? DylibKind::Weak
               : cmd == LC_REEXPORT_DYLIB ? DylibKind::Reexport : cmd == LC_LAZY_LOAD_DYLIB ? DylibKind::Lazy
               : cmd == LC_ID_DYLIB ? DylibKind::Id : DylibKind::Upward;
        img.dylibs.push_back(std::move(d));
        break;
      }
      case LC_RPATH: {
        uint32_t po = c.read<uint32_t>(8);
        if (po < 12 || po >= cmdsize) throw FormatError("bad rpath offset");
        img.rpaths.push_back(c.cstr(po, cmdsize, "rpath"));
        break;
      }
      case LC_UUID:
        c.need(8, 16, "uuid");
        img.uuid = formatUuid(c.bytes().data() + 8);
        break;
      case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
        for (int k = 0; k < 10; ++k) pend.dyldInfo[k] = c.read<uint32_t>(8 + k * 4);
        pend.haveDyldInfo = true;
        img.hasDyldInfo = true;
        if (pend.dyldInfo[9]) img.hasExportsTrie = true;
        break;
      case LC_DYLD_CHAINED_FIXUPS:
        pend.chainedOff = c.read<uint32_t>(8); pend.chainedSize = c.read<uint32_t>(12);
        pend.haveChained = true;
        break;
      case LC_DYLD_EXPORTS_TRIE: img.hasExportsTrie = true; break;
      case LC_FUNCTION_STARTS:
        pend.fnStartsOff = c.read<uint32_t>(8); pend.fnStartsSize = c.read<uint32_t>(12);
        pend.haveFnStarts = true;
        break;
      case LC_DATA_IN_CODE:
        pend.dicOff = c.read<uint32_t>(8); pend.dicSize = c.read<uint32_t>(12);
        pend.haveDic = true;
        break;
      case LC_MAIN: pend.mainEntryOff = c.read<uint64_t>(8); break;
      case LC_UNIXTHREAD: {
        uint32_t flavor = c.read<uint32_t>(8), count = c.read<uint32_t>(12);
        if (img.arch == Arch::ARM64 || img.arch == Arch::ARM64e) {
          if (flavor == 6 && count >= 68) pend.threadPc = c.read<uint64_t>(16 + 32 * 8);
        } else if (flavor == 1 && count >= 17) {
          pend.threadPc = c.read<uint32_t>(16 + 15 * 4);
        }
        break;
      }
      case LC_BUILD_VERSION:
        img.buildPlatform = c.read<uint32_t>(8); img.minOs = c.read<uint32_t>(12); img.sdk = c.read<uint32_t>(16);
        break;
      case LC_VERSION_MIN_IPHONEOS:
        img.buildPlatform = 2; img.minOs = c.read<uint32_t>(8); img.sdk = c.read<uint32_t>(12);
        break;
      case LC_ENCRYPTION_INFO: case LC_ENCRYPTION_INFO_64:
        img.cryptOff = c.read<uint32_t>(8); img.cryptSize = c.read<uint32_t>(12); img.cryptId = c.read<uint32_t>(16);
        break;
      case LC_CODE_SIGNATURE:
        img.hasCodeSignature = true;
        img.codeSigOff = c.read<uint32_t>(8); img.codeSigSize = c.read<uint32_t>(12);
        if (!r.inRange(img.codeSigOff, img.codeSigSize)) throw FormatError("code signature outside slice");
        break;
      default: break;  // unknown commands are skipped, not trusted
    }
    off += cmdsize;
  }

  // Segment sanity: no overlapping mappings.
  {
    std::vector<const Segment*> v;
    for (const auto& s : img.segments) if (s.vmsize) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->vmaddr < b->vmaddr; });
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i - 1]->vmaddr + v[i - 1]->vmsize > v[i]->vmaddr) throw FormatError("overlapping segments");
  }
  if (img.segments.empty()) throw FormatError("no segments");
  const uint64_t textBase = img.textBase();

  if (pend.mainEntryOff) img.entry = textBase + *pend.mainEntryOff;
  else if (pend.threadPc) img.entry = *pend.threadPc;

  if (pend.haveSymtab) {
    const uint32_t ent = img.is64 ? 16 : 12;
    if (pend.nsyms > kMaxSymbols) throw FormatError("too many symbols");
    r.need(pend.symoff, uint64_t(pend.nsyms) * ent, "symbol table");
    r.need(pend.stroff, pend.strsize, "string table");
    img.symbols.reserve(pend.nsyms);
    for (uint32_t i = 0; i < pend.nsyms; ++i) {
      uint64_t o = uint64_t(pend.symoff) + uint64_t(i) * ent;
      Symbol s;
      uint32_t strx = r.read<uint32_t>(o);
      s.type = r.read<uint8_t>(o + 4); s.sect = r.read<uint8_t>(o + 5); s.desc = r.read<uint16_t>(o + 6);
      s.value = img.is64 ? r.read<uint64_t>(o + 8) : r.read<uint32_t>(o + 8);
      if (strx) {
        if (strx >= pend.strsize) throw FormatError("symbol name index outside string table");
        s.name = r.cstr(uint64_t(pend.stroff) + strx, uint64_t(pend.stroff) + pend.strsize, "symbol name");
      }
      img.symbols.push_back(std::move(s));
    }
  }
  if (pend.haveDysymtab && pend.nindirect) {
    if (pend.nindirect > kMaxSymbols) throw FormatError("too many indirect symbols");
    r.need(pend.indirectOff, uint64_t(pend.nindirect) * 4, "indirect symbols");
    for (uint32_t i = 0; i < pend.nindirect; ++i) img.indirectSymbols.push_back(r.read<uint32_t>(uint64_t(pend.indirectOff) + i * 4));
  }

  if (!pend.haveDyldInfo && !pend.haveChained) synthesizeClassicBinds(img, importTable);
  if (pend.haveDyldInfo) {
    const uint32_t* d = pend.dyldInfo;
    if (d[1]) parseRebaseOpcodes(img, r, d[0], d[1]);
    if (d[3]) parseBindOpcodes(img, importTable, r, d[2], d[3], false);
    if (d[7]) parseBindOpcodes(img, importTable, r, d[6], d[7], true);
    // Weak-bind coalescing (d[4], d[5]) is not interpreted yet.
  }
  if (pend.haveChained && pend.chainedSize) parseChainedFixups(img, importTable, r, pend.chainedOff, pend.chainedSize);

  if (pend.haveFnStarts && pend.fnStartsSize) {
    r.need(pend.fnStartsOff, pend.fnStartsSize, "function starts");
    uint64_t p = pend.fnStartsOff, end = uint64_t(pend.fnStartsOff) + pend.fnStartsSize, addr = textBase;
    while (p < end) {
      uint64_t delta = r.uleb(p, end, "function start");
      if (!delta) break;
      addr += delta;
      img.functionStarts.push_back(addr);
    }
  }
  if (pend.haveDic && pend.dicSize) {
    r.need(pend.dicOff, pend.dicSize, "data in code");
    for (uint64_t o = pend.dicOff; o + 8 <= uint64_t(pend.dicOff) + pend.dicSize; o += 8)
      img.dataInCode.push_back({textBase + r.read<uint32_t>(o), r.read<uint16_t>(o + 4)});
  }
  return img;
}

Image parseFile(Bytes file) {
  auto slices = listSlices(file);
  auto choice = chooseSlice(slices);
  if (!choice.index) throw FormatError(choice.reason);
  return parseSlice(file, slices[*choice.index]);
}

}  // namespace radeki::macho
