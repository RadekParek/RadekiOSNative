// AArch64 instruction classification/encoding for the instruction classes the relinker cares about.
#pragma once
#include <cstdint>
#include <optional>

namespace radeki::a64 {

enum class Kind { Other, B, BL, BCond, CBZ, TBZ, ADR, ADRP, LdrLit, BR, BLR, RET, BRK, PacSign, PacAuth, PacStrip, PacBranch };

struct Insn {
  Kind kind = Kind::Other;
  int64_t imm = 0;   // PC-relative byte offset (ADRP: page delta * 4096)
  uint8_t rd = 0, rn = 0;
  uint16_t brk = 0;
  bool pcRel() const {
    return kind == Kind::B || kind == Kind::BL || kind == Kind::BCond || kind == Kind::CBZ || kind == Kind::TBZ ||
           kind == Kind::ADR || kind == Kind::ADRP || kind == Kind::LdrLit;
  }
};

inline int64_t sext(uint64_t v, unsigned bits) {
  uint64_t m = 1ull << (bits - 1);
  return int64_t((v & ((m << 1) - 1)) ^ m) - int64_t(m);
}

inline Insn decode(uint32_t w) {
  Insn i;
  i.rd = w & 31;
  i.rn = (w >> 5) & 31;
  if ((w & 0xFC000000) == 0x14000000) { i.kind = Kind::B; i.imm = sext(w & 0x3FFFFFF, 26) * 4; }
  else if ((w & 0xFC000000) == 0x94000000) { i.kind = Kind::BL; i.imm = sext(w & 0x3FFFFFF, 26) * 4; }
  else if ((w & 0xFF000010) == 0x54000000) { i.kind = Kind::BCond; i.imm = sext((w >> 5) & 0x7FFFF, 19) * 4; }
  else if ((w & 0x7E000000) == 0x34000000) { i.kind = Kind::CBZ; i.imm = sext((w >> 5) & 0x7FFFF, 19) * 4; }
  else if ((w & 0x7E000000) == 0x36000000) { i.kind = Kind::TBZ; i.imm = sext((w >> 5) & 0x3FFF, 14) * 4; }
  else if ((w & 0x9F000000) == 0x10000000) { i.kind = Kind::ADR; i.imm = sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21); }
  else if ((w & 0x9F000000) == 0x90000000) { i.kind = Kind::ADRP; i.imm = sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) * 4096; }
  else if ((w & 0x3B000000) == 0x18000000) { i.kind = Kind::LdrLit; i.imm = sext((w >> 5) & 0x7FFFF, 19) * 4; }
  else if ((w & 0xFFFFFC1F) == 0xD61F0000) i.kind = Kind::BR;
  else if ((w & 0xFFFFFC1F) == 0xD63F0000) i.kind = Kind::BLR;
  else if ((w & 0xFFFFFC1F) == 0xD65F0000) i.kind = Kind::RET;
  else if ((w & 0xFFE0001F) == 0xD4200000) { i.kind = Kind::BRK; i.brk = (w >> 5) & 0xFFFF; }
  else if (w == 0xD65F0BFF || w == 0xD65F0FFF) i.kind = Kind::PacAuth;
  else if ((w & 0xFFDFF800) == 0xD71F0800 || (w & 0xFFDFF800) == 0xD61F0800) i.kind = Kind::PacBranch;
  else if ((w & 0xFF200400) == 0xF8200400) i.kind = Kind::PacAuth;  // LDRAA/LDRAB
  else if ((w & 0xFFFFFBE0) == 0xDAC143E0) i.kind = Kind::PacStrip;
  else if ((w & 0xFFFFF01F) == 0xD503201F) {
    unsigned h = (w >> 5) & 0x7F;
    if (h == 8 || h == 10 || (h >= 24 && h <= 27)) i.kind = Kind::PacSign;
    else if (h == 12 || h == 14 || (h >= 28 && h <= 31)) i.kind = Kind::PacAuth;
    else if (h == 7) i.kind = Kind::PacStrip;
  }
  return i;
}

// Absolute target of a PC-relative instruction.
inline uint64_t targetOf(const Insn& i, uint64_t pc) {
  return i.kind == Kind::ADRP ? (pc & ~0xFFFull) + uint64_t(i.imm) : pc + uint64_t(i.imm);
}

inline std::optional<uint32_t> encodeB(uint64_t pc, uint64_t target, bool link) {
  int64_t d = int64_t(target - pc);
  if ((d & 3) || d < -(1ll << 27) || d >= (1ll << 27)) return std::nullopt;
  return (link ? 0x94000000u : 0x14000000u) | (uint32_t(d >> 2) & 0x3FFFFFF);
}

// Re-encode a PC-relative instruction at `pc` so it reaches `target`; nullopt if out of range/misaligned.
inline std::optional<uint32_t> withTarget(uint32_t w, uint64_t pc, uint64_t target) {
  Insn i = decode(w);
  auto fits = [](int64_t v, unsigned bits) { return v >= -(1ll << (bits - 1)) && v < (1ll << (bits - 1)); };
  int64_t d = int64_t(target - pc);
  switch (i.kind) {
    case Kind::B: case Kind::BL: return encodeB(pc, target, i.kind == Kind::BL);
    case Kind::BCond: case Kind::CBZ: case Kind::LdrLit:
      if ((d & 3) || !fits(d >> 2, 19)) return std::nullopt;
      return (w & ~(0x7FFFFu << 5)) | ((uint32_t(d >> 2) & 0x7FFFF) << 5);
    case Kind::TBZ:
      if ((d & 3) || !fits(d >> 2, 14)) return std::nullopt;
      return (w & ~(0x3FFFu << 5)) | ((uint32_t(d >> 2) & 0x3FFF) << 5);
    case Kind::ADR: {
      if (!fits(d, 21)) return std::nullopt;
      uint32_t u = uint32_t(d) & 0x1FFFFF;
      return (w & 0x9F00001F) | ((u & 3) << 29) | ((u >> 2) << 5);
    }
    case Kind::ADRP: {
      int64_t pages = int64_t(target >> 12) - int64_t(pc >> 12);
      if (!fits(pages, 21)) return std::nullopt;
      uint32_t u = uint32_t(pages) & 0x1FFFFF;
      return (w & 0x9F00001F) | ((u & 3) << 29) | ((u >> 2) << 5);
    }
    default: return std::nullopt;
  }
}

}  // namespace radeki::a64
