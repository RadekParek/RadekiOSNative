// Builds small, legal, synthetic ARM64 Mach-O files for tests (no Apple code involved).
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "arm64/arm64.h"

namespace synth {
using Buf = std::vector<uint8_t>;
struct Opts {
  int chainedFmt = 0;        // 0 = dyld-info opcodes, else chained pointer format (2, 6, 9)
  uint32_t cpusub = 0;       // 2 => arm64e
  uint32_t cryptid = 0;
  bool flatImport = false;   // add one extra import bound with ordinal -2 (flat namespace)
};
constexpr uint64_t kBase = 0x100000000ull, kText = kBase + 0x1000, kStr = kBase + 0x2010, kData = kBase + 0x4000;
constexpr int kHelperIdx = 15;

inline void p32(Buf& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
inline void p64(Buf& b, size_t o, uint64_t v) { std::memcpy(&b[o], &v, 8); }
inline void put(Buf& c, uint32_t v) { c.insert(c.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
inline void put64(Buf& c, uint64_t v) { c.insert(c.end(), (uint8_t*)&v, (uint8_t*)&v + 8); }
inline void putName(Buf& c, const char* n) { char t[16] = {}; std::strncpy(t, n, 16); c.insert(c.end(), t, t + 16); }
inline void pad(Buf& c, size_t n) { while (c.size() % n) c.push_back(0); }

inline std::vector<uint32_t> code() {
  using namespace radeki::a64;
  auto adrp = [](int rd, uint64_t pc, uint64_t tgt) { return *withTarget(0x90000000u | rd, pc, tgt); };
  uint64_t t = kText;
  std::vector<uint32_t> c = {
      0xA9BF7BFD, 0x910003FD,
      adrp(0, t + 8, kStr), 0x91000000u | (0x10 << 10),                 // adrp x0,str ; add x0,x0,#0x10
      adrp(8, t + 16, kData), 0xF9400108,                               // adrp x8,got ; ldr x8,[x8]
      0xD63F0100,                                                       // blr x8
      *encodeB(t + 28, t + kHelperIdx * 4, true),                       // bl helper
      adrp(1, t + 32, kData), 0xF9400820, 0xF9400028, 0xD63F0100,       // x1=data; ldr x0,[x1,#0x10]; ldr x8,[x1]; blr x8
      0x528000E0,                                                       // mov w0,#7
      0xA8C17BFD, 0xD65F03C0,                                           // ldp ; ret
      0x52800000, 0xD65F03C0};                                          // helper: mov w0,#0 ; ret
  return c;
}

inline Buf build(const Opts& o = {}) {
  Buf f(0x8180, 0);
  auto c = code();
  for (size_t i = 0; i < c.size(); ++i) p32(f, 0x1000 + i * 4, c[i]);
  std::strcpy((char*)&f[0x2010], "hello radeki");
  const size_t le = 0x8000;
  Buf rebase = {0x11, 0x22, 0x10, 0x51, 0x00};
  Buf bind = {0x11, 0x40, '_', 'p', 'u', 't', 's', 0, 0x51, 0x72, 0x00, 0x90,
              0x40, '_', 'm', 'i', 's', 's', 'i', 'n', 'g', '_', 'f', 'n', 0, 0x72, 0x08, 0x90};
  if (o.flatImport) {
    // SET_DYLIB_ORDINAL_IMM(0xE) encodes -2: the flat-namespace special ordinal, which makes
    // the loader look for the provider in any loaded image rather than in a named dylib.
    const char* flat = "_flat_sym";
    bind.push_back(0x3E);
    bind.push_back(0x40);
    bind.insert(bind.end(), flat, flat + std::strlen(flat));
    bind.push_back(0);
    bind.push_back(0x51);        // SET_TYPE_IMM(1): pointer
    bind.push_back(0x72); bind.push_back(0x18);  // __DATA + 0x18
    bind.push_back(0x90);        // DO_BIND
  }
  bind.push_back(0x00);          // DONE
  Buf fstarts = {0x80, 0x20, 0x3C, 0x00};  // 0x1000, +60
  std::memcpy(&f[le + 0x00], rebase.data(), rebase.size());
  std::memcpy(&f[le + 0x20], bind.data(), bind.size());
  std::memcpy(&f[le + 0x60], fstarts.data(), fstarts.size());
  uint32_t chainedSize = 0;
  if (o.chainedFmt) {
    Buf b(112, 0);
    p32(b, 4, 32); p32(b, 8, 80); p32(b, 12, 88); p32(b, 16, 2); p32(b, 20, 1); p32(b, 24, 0);
    p32(b, 32, 4); p32(b, 32 + 4 + 2 * 4, 24);
    p32(b, 56, 24); b[60] = 0x00; b[61] = 0x40; b[62] = o.chainedFmt; b[63] = 0;
    p64(b, 64, 0x4000); b[76] = 1;
    p32(b, 80, 1 | (1u << 9)); p32(b, 84, 1 | (7u << 9));
    std::memcpy(&b[89], "_puts", 6); std::memcpy(&b[95], "_missing_fn", 12);
    std::memcpy(&f[le + 0x80], b.data(), b.size());
    chainedSize = 112;
    uint64_t s0, s1, s2;
    if (o.chainedFmt == 9) {
      s0 = 0 | (1ull << 51) | (1ull << 62); s1 = 1 | (1ull << 51) | (1ull << 62);
      s2 = 0x2010ull | (0x1234ull << 32) | (1ull << 63);
    } else {
      s0 = 0 | (2ull << 51) | (1ull << 63); s1 = 1 | (2ull << 51) | (1ull << 63);
      s2 = o.chainedFmt == 6 ? 0x2010ull : kStr;
    }
    p64(f, 0x4000, s0); p64(f, 0x4008, s1); p64(f, 0x4010, s2);
  } else {
    p64(f, 0x4010, kStr);  // pointer stored as unslid vmaddr; rebase opcodes mark it
  }
  // symtab: strtab "\0_helper\0_main\0_puts\0_missing_fn\0"
  const char strtab[] = "\0_helper\0_main\0_puts\0_missing_fn";
  std::memcpy(&f[le + 0x140], strtab, sizeof strtab);
  struct N { uint32_t x; uint8_t t, s; uint16_t d; uint64_t v; };
  N syms[4] = {{1, 0x0E, 1, 0, kText + kHelperIdx * 4}, {9, 0x0F, 1, 0, kText}, {15, 0x01, 0, 0x100, 0}, {21, 0x01, 0, 0x100, 0}};
  std::memcpy(&f[le + 0x100], syms, sizeof syms);

  Buf lc;
  auto seg = [&](const char* n, uint64_t va, uint64_t vs, uint64_t fo, uint64_t fs, uint32_t prot, uint32_t nsect) {
    put(lc, 0x19); put(lc, 72 + 80 * nsect); putName(lc, n);
    put64(lc, va); put64(lc, vs); put64(lc, fo); put64(lc, fs);
    put(lc, prot); put(lc, prot); put(lc, nsect); put(lc, 0);
  };
  auto sect = [&](const char* n, const char* sg, uint64_t a, uint64_t sz, uint32_t off, uint32_t fl) {
    putName(lc, n); putName(lc, sg); put64(lc, a); put64(lc, sz);
    put(lc, off); put(lc, 2); put(lc, 0); put(lc, 0); put(lc, fl); put(lc, 0); put(lc, 0); put(lc, 0);
  };
  seg("__PAGEZERO", 0, kBase, 0, 0, 0, 0);
  seg("__TEXT", kBase, 0x4000, 0, 0x4000, 5, 2);
  sect("__text", "__TEXT", kText, c.size() * 4, 0x1000, 0x80000400);
  sect("__cstring", "__TEXT", kStr, 16, 0x2010, 2);
  seg("__DATA", kData, 0x4000, 0x4000, 0x4000, 3, 2);
  sect("__got", "__DATA", kData, 16, 0x4000, 6);
  sect("__data", "__DATA", kData + 0x10, o.flatImport ? 16 : 8, 0x4010, 0);
  seg("__LINKEDIT", kBase + 0x8000, 0x4000, le, 0x180, 1, 0);
  uint32_t ncmds = 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1;  // 4 segs + fixups + symtab + dysymtab + dylib + main + uuid + fstarts
  if (o.chainedFmt) { put(lc, 0x80000034); put(lc, 16); put(lc, le + 0x80); put(lc, chainedSize); }
  else {
    put(lc, 0x80000022); put(lc, 48);
    uint32_t v[10] = {le, 5, le + 0x20, (uint32_t)bind.size(), 0, 0, 0, 0, 0, 0};
    for (uint32_t x : v) put(lc, x);
  }
  put(lc, 2); put(lc, 24); put(lc, le + 0x100); put(lc, 4); put(lc, le + 0x140); put(lc, sizeof strtab);
  put(lc, 0xB); put(lc, 80);
  uint32_t dy[18] = {0, 1, 1, 1, 2, 2};
  for (uint32_t x : dy) put(lc, x);
  const char* ln = "/usr/lib/libSystem.B.dylib";
  put(lc, 0xC); put(lc, 56); put(lc, 24); put(lc, 0); put(lc, 0x10000); put(lc, 0x10000);
  size_t s0 = lc.size(); lc.insert(lc.end(), ln, ln + std::strlen(ln) + 1); lc.resize(s0 + 32);
  put(lc, 0x80000028); put(lc, 24); put64(lc, 0x1000); put64(lc, 0);
  put(lc, 0x1B); put(lc, 24); for (int i = 0; i < 16; ++i) lc.push_back(uint8_t(0xA0 + i));
  put(lc, 0x26); put(lc, 16); put(lc, le + 0x60); put(lc, 4);
  if (o.cryptid) { put(lc, 0x2C); put(lc, 24); put(lc, 0x1000); put(lc, 0x1000); put(lc, o.cryptid); put(lc, 0); ++ncmds; }

  p32(f, 0, 0xFEEDFACF); p32(f, 4, 0x0100000C); p32(f, 8, o.cpusub); p32(f, 12, 2);
  p32(f, 16, ncmds); p32(f, 20, (uint32_t)lc.size()); p32(f, 24, 0x200085); p32(f, 28, 0);
  std::memcpy(&f[32], lc.data(), lc.size());
  return f;
}

// Wrap slices in a fat container (big-endian header).
inline Buf fat(const std::vector<std::pair<std::pair<uint32_t, uint32_t>, Buf>>& slices) {
  Buf out(8 + 20 * slices.size(), 0);
  auto be = [&](size_t o, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(&out[o], &v, 4); };
  be(0, 0xCAFEBABE); be(4, (uint32_t)slices.size());
  for (size_t i = 0; i < slices.size(); ++i) {
    while (out.size() % 0x4000) out.push_back(0);
    size_t off = out.size();
    out.insert(out.end(), slices[i].second.begin(), slices[i].second.end());
    size_t e = 8 + 20 * i;
    be(e, slices[i].first.first); be(e + 4, slices[i].first.second); be(e + 8, (uint32_t)off);
    be(e + 12, (uint32_t)slices[i].second.size()); be(e + 16, 14);
  }
  return out;
}

// ---------------------------------------------------------------------------------------
// Layout of the Objective-C bearing synthetic image. Every metadata blob sits inside
// __objc_data so the parser can read it, and every absolute pointer into the image is
// recorded as a rebase slot, which is what makes the metadata walkable after a slide.
// ---------------------------------------------------------------------------------------
namespace objcimg {
constexpr uint32_t kTextFileOff = 0x1000;
constexpr uint32_t kTextInsns = 10;               // 5 real instructions + 5 RET stubs for method IMPs
constexpr uint32_t kMethnameSec = 0x100;          // section offsets inside __DATA
constexpr uint32_t kClasslistSec = 0x400;
constexpr uint32_t kCatlistSec = 0x408;
constexpr uint32_t kProtolistSec = 0x410;
constexpr uint32_t kObjcDataSec = 0x800;
constexpr uint32_t kSlotClassPtr = 0x000;         // __data[0]: rebased pointer to the class object
constexpr uint32_t kSlotImport = 0x018;           // __data[3]: bound to _missing_fn
constexpr uint32_t kClassList = 0x400;            // == kClasslistSec
constexpr uint32_t kCatList = 0x408;
constexpr uint32_t kProtoList = 0x410;
// blobs
constexpr uint32_t kWidgetClass = 0x800;
constexpr uint32_t kWidgetMeta = 0x830;
constexpr uint32_t kBaseClass = 0x860;
constexpr uint32_t kBaseMeta = 0x890;
constexpr uint32_t kBaseRo = 0x8C0;
constexpr uint32_t kWidgetRo = 0x910;
constexpr uint32_t kWidgetMethods = 0x958;
constexpr uint32_t kWidgetClassMethods = 0x990;
constexpr uint32_t kWidgetMetaRo = 0x9B0;
constexpr uint32_t kBaseMetaRo = 0xA00;
constexpr uint32_t kProtocol = 0xA48;
constexpr uint32_t kProtocolMethods = 0xA88;
constexpr uint32_t kCategory = 0xAA8;
constexpr uint32_t kCategoryMethods = 0xAD0;
constexpr uint32_t kProtocolList = 0xB20;
constexpr uint32_t kIvarList = 0xAF0;
constexpr uint32_t kPropertyList = 0xB0C;
constexpr uint32_t kIvarOffsetSlot = 0xB1C;
// implementation addresses (RET stubs inside __text)
constexpr uint64_t kImp0 = kBase + 0x1014;
// string pool offsets inside __objc_methname
constexpr uint32_t sDoWork = 0x00, sInit = 0x08, sShared = 0x10, sRun = 0x18, sVoidTypes = 0x20,
                   sClassTypes = 0x28, sWidget = 0x30, sModelBase = 0x38, sProto = 0x48,
                   sCategory = 0x58, sCounter = 0x68, sName = 0x70, sAttrs = 0x78, sTypeQ = 0x90;
// method/ivar/property list entry sizes
constexpr uint32_t kMethodEnt = 24, kIvarEnt = 20, kPropEnt = 8;
constexpr uint64_t kDataSeg = kData;              // == 0x100004000
}  // namespace objcimg

// A synthetic ARM64 image carrying real Objective-C metadata: one class (Widget, deriving
// from ModelBase), a class method, an ivar, a property, a protocol and a category.
// It imports _missing_fn (like synth::build) and exports _OBJC_CLASS_$_Widget as a symbol.
inline Buf objcImage() {
  using namespace radeki::a64;
  using namespace objcimg;
  Buf f(0xC200, 0);
  const uint64_t D = kDataSeg;                     // __DATA vmaddr
  std::vector<uint32_t> rebaseSlots;               // __DATA-relative offsets of absolute pointers
  uint32_t rebaseSize = 0, bindSize = 0;

  // Pointer helper: writes an image pointer and remembers the slot for the rebase opcodes.
  auto ptr = [&](uint32_t dataOff, uint64_t value, bool rebase = true) {
    p64(f, 0x4000 + dataOff, value);
    if (rebase && value) rebaseSlots.push_back(dataOff);
  };
  auto u32at = [&](uint32_t dataOff, uint32_t v) { p32(f, 0x4000 + dataOff, v); };
  auto strAddr = [&](uint32_t poolOff) { return D + kMethnameSec + poolOff; };
  auto rel = [&](uint32_t entryOff, uint32_t fieldOff, uint64_t target) {
    p32(f, 0x4000 + entryOff + fieldOff, uint32_t(int32_t(int64_t(target) - int64_t(D + entryOff + fieldOff))));
  };

  // --- __text: load the class object pointer, then report a fixed exit code --------------
  std::vector<uint32_t> code = {
      *withTarget(0x90000000u | 0, kText, D),                            // adrp x0, __DATA
      0x91200000u,                                                       // add x0, x0, #0x800
      0xF9400001,                                                        // ldr x1, [x0]
      0x528000A0,                                                        // mov w0, #5
      0xD65F03C0};                                                       // ret
  for (int i = 0; i < 5; ++i) code.push_back(0xD65F03C0);                // RET stubs for method imps
  for (size_t i = 0; i < code.size(); ++i) p32(f, kTextFileOff + i * 4, code[i]);
  std::strcpy((char*)&f[0x2000], "widgets");

  // --- strings ---------------------------------------------------------------------------
  auto putStr = [&](uint32_t off, const char* s) {
    std::memcpy(&f[0x4000 + kMethnameSec + off], s, std::strlen(s) + 1);
  };
  putStr(sDoWork, "doWork");
  putStr(sInit, "init");
  putStr(sShared, "shared");
  putStr(sRun, "run");
  putStr(sVoidTypes, "v@:");
  putStr(sClassTypes, "@:");
  putStr(sWidget, "Widget");
  putStr(sModelBase, "ModelBase");
  putStr(sProto, "RadekiProto");
  putStr(sCategory, "RadekiCat");
  putStr(sCounter, "counter");
  putStr(sName, "name");
  putStr(sAttrs, "T@\"NSString\",&,N");
  putStr(sTypeQ, "Q");

  // --- class objects (isa, superclass, cache, class_data_bits_t -> class_ro_t) ------------
  ptr(kWidgetClass + 0, D + kWidgetMeta);
  ptr(kWidgetClass + 8, D + kBaseClass);
  ptr(kWidgetClass + 0x20, D + kWidgetRo);
  ptr(kWidgetMeta + 0, D + kWidgetMeta);           // metaclass isa points at itself
  ptr(kWidgetMeta + 8, D + kBaseClass);
  ptr(kWidgetMeta + 0x20, D + kWidgetMetaRo);
  ptr(kBaseClass + 0, D + kBaseMeta);
  ptr(kBaseClass + 0x20, D + kBaseRo);
  ptr(kBaseMeta + 0, D + kBaseMeta);
  ptr(kBaseMeta + 0x20, D + kBaseMetaRo);

  // --- class_ro_t ------------------------------------------------------------------------
  u32at(kBaseRo + 8, 8);                           // instanceSize
  ptr(kBaseRo + 24, strAddr(sModelBase));          // name
  u32at(kWidgetRo + 8, 24);                        // instanceSize
  ptr(kWidgetRo + 24, strAddr(sWidget));
  ptr(kWidgetRo + 32, D + kWidgetMethods);
  ptr(kWidgetRo + 40, D + kProtocolList);
  ptr(kWidgetRo + 48, D + kIvarList);
  ptr(kWidgetRo + 64, D + kPropertyList);
  u32at(kWidgetMetaRo + 8, 24);
  ptr(kWidgetMetaRo + 24, strAddr(sWidget));
  ptr(kWidgetMetaRo + 32, D + kWidgetClassMethods);
  u32at(kBaseMetaRo + 8, 8);
  ptr(kBaseMetaRo + 24, strAddr(sModelBase));

  // --- method lists (classic 24-byte entries, relative name/type, absolute IMP) -----------
  // The classic encoding stores absolute pointers (all of them rebased); the relative one
  // stores three int32 deltas from the field's own address. Both are exercised here.
  auto methodList = [&](uint32_t listOff, const std::vector<std::tuple<uint32_t, uint32_t, uint64_t>>& ms, bool relative) {
    const uint32_t ent = relative ? 12 : kMethodEnt;
    p32(f, 0x4000 + listOff, ent);
    p32(f, 0x4000 + listOff + 4, (uint32_t)ms.size());
    for (size_t i = 0; i < ms.size(); ++i) {
      uint32_t e = listOff + 8 + uint32_t(i) * ent;
      if (relative) {
        rel(e, 0, strAddr(std::get<0>(ms[i])));
        rel(e, 4, strAddr(std::get<1>(ms[i])));
        rel(e, 8, std::get<2>(ms[i]));
      } else {
        ptr(e, strAddr(std::get<0>(ms[i])));
        ptr(e + 8, strAddr(std::get<1>(ms[i])));
        ptr(e + 16, std::get<2>(ms[i]));
      }
    }
  };
  methodList(kWidgetMethods, {{sDoWork, sVoidTypes, kImp0}, {sInit, sVoidTypes, kImp0}}, false);
  methodList(kWidgetClassMethods, {{sShared, sClassTypes, kImp0}}, true);
  methodList(kProtocolMethods, {{sRun, sVoidTypes, kImp0}}, false);
  methodList(kCategoryMethods, {{sRun, sVoidTypes, kImp0}}, false);

  // --- ivar list (relative 20-byte entries) ---------------------------------------------
  u32at(kIvarList, kIvarEnt);
  u32at(kIvarList + 4, 1);
  rel(kIvarList + 8, 0, D + kIvarOffsetSlot);
  rel(kIvarList + 8, 4, strAddr(sCounter));
  rel(kIvarList + 8, 8, strAddr(sTypeQ));
  u32at(kIvarList + 8 + 12, 3);                    // alignment
  u32at(kIvarList + 8 + 16, 8);                    // size
  u32at(kIvarOffsetSlot, 8);                       // actual ivar offset

  // --- property list (relative 8-byte entries) ------------------------------------------
  u32at(kPropertyList, kPropEnt);
  u32at(kPropertyList + 4, 1);
  rel(kPropertyList + 8, 0, strAddr(sName));
  rel(kPropertyList + 8, 4, strAddr(sAttrs));

  // --- protocol_t -----------------------------------------------------------------------
  ptr(kProtocol + 8, strAddr(sProto));
  ptr(kProtocol + 24, D + kProtocolMethods);
  u32at(kProtocol + 56, 0);                        // size / flags slot
  ptr(kProtocolList, 1, /*rebase=*/false);         // protocol_list_t: absolute count...
  ptr(kProtocolList + 8, D + kProtocol);           // ...followed by the protocol pointers

  // --- category_t -----------------------------------------------------------------------
  ptr(kCategory + 0, strAddr(sCategory));
  ptr(kCategory + 8, D + kWidgetClass);            // the class being extended
  ptr(kCategory + 16, D + kCategoryMethods);

  // --- the lists the parser starts from --------------------------------------------------
  ptr(kClassList, D + kWidgetClass);
  ptr(kCatList, D + kCategory);
  ptr(kProtoList, D + kProtocol);
  ptr(kSlotClassPtr, D + kWidgetClass);            // rebased __data pointer

  // --- dyld info: rebase opcodes for every recorded slot, one bind for _missing_fn ---------
  uint32_t ro = 0xC000, bo = 0xC110;
  {
    Buf rb;
    rb.push_back(0x11);                            // SET_TYPE_IMM(1): pointer
    for (uint32_t off : rebaseSlots) {
      rb.push_back(0x22);                          // SET_SEGMENT_AND_OFFSET_ULEB, segment 2 (__DATA)
      do { uint8_t b = uint8_t(off & 0x7F); off >>= 7; rb.push_back(b | (off ? 0x80 : 0)); } while (off);
      rb.push_back(0x51);                          // DO_REBASE_IMM_TIMES(1)
    }
    rb.push_back(0x00);                            // DONE
    if (rb.size() > 0x100) throw std::runtime_error("rebase opcodes overflow the test image");
    std::memcpy(&f[ro], rb.data(), rb.size());
    rebaseSize = (uint32_t)rb.size();
    Buf bb = {0x11, 0x40, '_', 'm', 'i', 's', 's', 'i', 'n', 'g', '_', 'f', 'n', 0,
              0x51, 0x72, 0x18, 0x90, 0x00};
    std::memcpy(&f[bo], bb.data(), bb.size());
    bindSize = (uint32_t)bb.size();
  }

  // --- symbols: _main, the _missing_fn import, and the exported class symbol --------------
  const char strtab[] = "\0_main\0_missing_fn\0_OBJC_CLASS_$_Widget";
  std::memcpy(&f[0xC1C0], strtab, sizeof strtab);
  struct N { uint32_t x; uint8_t t, s; uint16_t d; uint64_t v; };
  N syms[3] = {{1, 0x0F, 1, 0, kText},
               {7, 0x01, 0, 0x100, 0},
               {19, 0x0F, 8, 0, D + kWidgetClass}};
  std::memcpy(&f[0xC180], syms, sizeof syms);

  // --- load commands ---------------------------------------------------------------------
  Buf lc;
  auto put2 = [&](uint32_t v) { put(lc, v); };
  auto seg = [&](const char* n, uint64_t va, uint64_t vs, uint64_t fo, uint64_t fs, uint32_t prot, uint32_t nsect) {
    put2(0x19); put2(72 + 80 * nsect); putName(lc, n);
    put64(lc, va); put64(lc, vs); put64(lc, fo); put64(lc, fs);
    put2(prot); put2(prot); put2(nsect); put2(0);
  };
  auto sect = [&](const char* n, const char* sg, uint64_t a, uint64_t sz, uint32_t off, uint32_t fl) {
    putName(lc, n); putName(lc, sg); put64(lc, a); put64(lc, sz);
    put2(off); put2(3); put2(0); put2(0); put2(fl); put2(0); put2(0); put2(0);
  };
  seg("__PAGEZERO", 0, kBase, 0, 0, 0, 0);
  seg("__TEXT", kBase, 0x4000, 0, 0x4000, 5, 2);
  sect("__text", "__TEXT", kText, kTextInsns * 4, kTextFileOff, 0x80000400);
  sect("__cstring", "__TEXT", kBase + 0x2000, 8, 0x2000, 2);
  seg("__DATA", D, 0x8000, 0x4000, 0x8000, 3, 6);
  sect("__data", "__DATA", D + kSlotClassPtr, 0x20, 0x4000 + kSlotClassPtr, 0);
  sect("__objc_methname", "__DATA", D + kMethnameSec, 0x200, 0x4000 + kMethnameSec, 2);
  sect("__objc_classlist", "__DATA", D + kClasslistSec, 8, 0x4000 + kClasslistSec, 0);
  sect("__objc_catlist", "__DATA", D + kCatlistSec, 8, 0x4000 + kCatlistSec, 0);
  sect("__objc_protolist", "__DATA", D + kProtolistSec, 8, 0x4000 + kProtolistSec, 0);
  sect("__objc_data", "__DATA", D + kObjcDataSec, 0x400, 0x4000 + kObjcDataSec, 0);
  seg("__LINKEDIT", kBase + 0xC000, 0x4000, 0xC000, 0x200, 1, 0);
  const uint32_t ncmds = 11;  // 4 segments, dyld_info, symtab, dysymtab, dylib, main, uuid, fnstarts
  put2(0x80000022); put2(48);
  {
    uint32_t v[10] = {ro, rebaseSize, bo, bindSize, 0, 0, 0, 0, 0, 0};
    for (uint32_t x : v) put2(x);
  }
  put2(2); put2(24); put2(0xC180); put2(3); put2(0xC1C0); put2(sizeof strtab);
  put2(0xB); put2(80);
  {
    uint32_t dy[18] = {0, 0, 0, 2, 2, 1};
    for (uint32_t x : dy) put2(x);
  }
  {
    const char* ln = "/usr/lib/libSystem.B.dylib";
    put2(0xC); put2(56); put2(24); put2(0); put2(0x10000); put2(0x10000);
    size_t s0 = lc.size(); lc.insert(lc.end(), ln, ln + std::strlen(ln) + 1); lc.resize(s0 + 32);
  }
  put2(0x80000028); put2(24); put64(lc, kTextFileOff); put64(lc, 0);
  put2(0x1B); put2(24);
  for (int i = 0; i < 16; ++i) lc.push_back(uint8_t(0x10 + i));
  put2(0x26); put2(16); put2(0xC140); put2(4);
  {  // one function start: delta 0x1000 from __TEXT, then the terminating zero delta
    const uint8_t fs[4] = {0x80, 0x20, 0x00, 0x00};
    std::memcpy(&f[0xC140], fs, sizeof fs);
  }

  p32(f, 0, 0xFEEDFACF); p32(f, 4, 0x0100000C); p32(f, 8, 0); p32(f, 12, 2);
  p32(f, 16, ncmds); p32(f, 20, (uint32_t)lc.size()); p32(f, 24, 0x200085); p32(f, 28, 0);
  std::memcpy(&f[32], lc.data(), lc.size());
  return f;
}

// A minimal ARM64 dylib that stands in for a shared library: it exports _missing_fn
// (a function returning 42) and _flat_sym, and claims the install name libSystem.B.dylib so
// two-level imports from the synthetic test images can bind to it.
inline Buf dylib() {
  using namespace radeki::a64;
  Buf f(0x4180, 0);
  auto code = [&](size_t i, uint32_t w) { p32(f, 0x1000 + i * 4, w); };
  code(0, 0x52800540);  // mov w0, #42
  code(1, 0xD65F03C0);  // ret
  code(2, 0xD65F03C0);  // _flat_sym sits here
  const char strtab[] = "\0_missing_fn\0_flat_sym";
  std::memcpy(&f[0x4140], strtab, sizeof strtab);
  struct N { uint32_t x; uint8_t t, s; uint16_t d; uint64_t v; };
  N syms[2] = {{1, 0x0F, 1, 0, 0x100001000}, {13, 0x0F, 1, 0, 0x100001008}};
  std::memcpy(&f[0x4100], syms, sizeof syms);

  Buf lc;
  auto seg = [&](const char* n, uint64_t va, uint64_t vs, uint64_t fo, uint64_t fs, uint32_t prot, uint32_t nsect) {
    put(lc, 0x19); put(lc, 72 + 80 * nsect); putName(lc, n);
    put64(lc, va); put64(lc, vs); put64(lc, fo); put64(lc, fs);
    put(lc, prot); put(lc, prot); put(lc, nsect); put(lc, 0);
  };
  auto sect = [&](const char* n, const char* sg, uint64_t a, uint64_t sz, uint32_t off, uint32_t fl) {
    putName(lc, n); putName(lc, sg); put64(lc, a); put64(lc, sz);
    put(lc, off); put(lc, 3); put(lc, 0); put(lc, 0); put(lc, fl); put(lc, 0); put(lc, 0); put(lc, 0);
  };
  seg("__PAGEZERO", 0, kBase, 0, 0, 0, 0);
  seg("__TEXT", kBase, 0x4000, 0, 0x4000, 5, 1);
  sect("__text", "__TEXT", 0x100001000, 12, 0x1000, 0x80000400);
  seg("__LINKEDIT", kBase + 0x4000, 0x4000, 0x4000, 0x180, 1, 0);
  const char* id = "/usr/lib/libSystem.B.dylib";
  put(lc, 0xD); put(lc, 56); put(lc, 24); put(lc, 0); put(lc, 0x10000); put(lc, 0x10000);
  size_t s0 = lc.size(); lc.insert(lc.end(), id, id + std::strlen(id) + 1); lc.resize(s0 + 32);
  put(lc, 2); put(lc, 24); put(lc, 0x4100); put(lc, 2); put(lc, 0x4140); put(lc, sizeof strtab);
  put(lc, 0xB); put(lc, 80);
  uint32_t dy[18] = {0, 0, 0, 2, 2, 0};
  for (uint32_t x : dy) put(lc, x);
  put(lc, 0x1B); put(lc, 24);
  for (int i = 0; i < 16; ++i) lc.push_back(uint8_t(0x40 + i));

  p32(f, 0, 0xFEEDFACF); p32(f, 4, 0x0100000C); p32(f, 8, 0); p32(f, 12, 6);  // MH_DYLIB
  p32(f, 16, 6); p32(f, 20, (uint32_t)lc.size()); p32(f, 24, 0x200085); p32(f, 28, 0);
  std::memcpy(&f[32], lc.data(), lc.size());
  return f;
}

}  // namespace synth
