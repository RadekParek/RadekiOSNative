// Tiny AArch64 interpreter used ONLY to verify relinked images in host tests.
// The production runtime executes the original AArch64 natively; this is a test oracle.
#pragma once
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

#include "arm64/arm64.h"
#include "relinker/relinker.h"

struct MiniCpu {
  uint64_t x[32] = {}, pc = 0, sp = 0;
  const radeki::relinker::LinkedImage* img;
  std::vector<uint8_t> stack = std::vector<uint8_t>(0x10000);
  static constexpr uint64_t kStackBase = 0x7FFE0000, kSentinel = 0xDEAD0000;
  std::map<uint64_t, std::function<void(MiniCpu&)>> hooks;
  std::string out, trap;

  uint8_t* mem(uint64_t a, size_t n) {
    if (a >= kStackBase && a + n <= kStackBase + stack.size()) return &stack[a - kStackBase];
    if (!img->contains(a, n)) throw std::runtime_error("bad memory access");
    return const_cast<uint8_t*>(img->memory.data()) + (a - img->loadBase);
  }
  uint64_t r64(uint64_t a) { uint64_t v; memcpy(&v, mem(a, 8), 8); return v; }
  void w64(uint64_t a, uint64_t v) { memcpy(mem(a, 8), &v, 8); }
  std::string cstr(uint64_t a) { std::string s; while (char c = *mem(a, 1)) { s += c; ++a; } return s; }
  uint64_t reg(int n) { return n == 31 ? 0 : x[n]; }
  uint64_t regSp(int n) { return n == 31 ? sp : x[n]; }
  void setSp(int n, uint64_t v) { if (n == 31) sp = v; else x[n] = v; }

  // Returns w0 of the called function; `trap` set if an unresolved-import BRK was hit.
  int run(uint64_t entry) {
    pc = entry; sp = kStackBase + stack.size() - 16; x[30] = kSentinel;
    for (int steps = 0; steps < 100000; ++steps) {
      if (pc == kSentinel) return int(x[0]);
      auto h = hooks.find(pc);
      if (h != hooks.end()) { h->second(*this); pc = x[30]; continue; }
      uint32_t w; memcpy(&w, mem(pc, 4), 4);
      auto i = radeki::a64::decode(w);
      using K = radeki::a64::Kind;
      uint64_t next = pc + 4;
      switch (i.kind) {
        case K::BRK:
          for (auto& t : img->traps) if (t.addr == pc) trap = t.symbol;
          if (trap.empty()) trap = "unknown brk";
          return -1;
        case K::B: next = radeki::a64::targetOf(i, pc); break;
        case K::BL: x[30] = pc + 4; next = radeki::a64::targetOf(i, pc); break;
        case K::ADR: case K::ADRP: if (i.rd != 31) x[i.rd] = radeki::a64::targetOf(i, pc); break;
        case K::BLR: x[30] = pc + 4; next = reg(i.rn); break;
        case K::BR: next = reg(i.rn); break;
        case K::RET: next = reg(i.rn); break;
        case K::LdrLit: if (i.rd != 31) x[i.rd] = r64(radeki::a64::targetOf(i, pc)); break;
        default:
          if ((w & 0xFFC00000) == 0x91000000 || (w & 0xFFC00000) == 0xD1000000) {  // ADD/SUB imm (64-bit)
            uint64_t imm = ((w >> 10) & 0xFFF) << ((w >> 22 & 1) ? 12 : 0);
            uint64_t b = regSp(i.rn);
            setSp(i.rd, (w & 0x40000000) ? b - imm : b + imm);
          } else if ((w & 0xFFC00000) == 0xF9400000) { x[i.rd] = r64(regSp(i.rn) + (((w >> 10) & 0xFFF) << 3)); }
          else if ((w & 0xFFC00000) == 0xF9000000) { w64(regSp(i.rn) + (((w >> 10) & 0xFFF) << 3), reg(i.rd)); }
          else if ((w & 0x7F800000) == 0x52800000) { if (i.rd != 31) x[i.rd] = (w >> 5) & 0xFFFF; }  // MOVZ
          else if ((w & 0xFE000000) == 0xA8000000 && (w & 0xC0000000) == 0x80000000) {  // STP/LDP 64-bit
            unsigned mode = (w >> 23) & 7; bool ld = (w >> 22) & 1;
            int64_t off = radeki::a64::sext((w >> 15) & 0x7F, 7) * 8;
            int t2 = (w >> 10) & 31, rn = (w >> 5) & 31, t = w & 31;
            uint64_t base = regSp(rn), addr = mode == 1 ? base : base + off;
            if (ld) { x[t] = r64(addr); x[t2] = r64(addr + 8); } else { w64(addr, reg(t)); w64(addr + 8, reg(t2)); }
            if (mode == 1 || mode == 3) setSp(rn, base + off);
          } else if (w != 0xD503201F) {
            char b[64]; snprintf(b, sizeof b, "unsupported insn %08x at %llx", w, (unsigned long long)pc);
            throw std::runtime_error(b);
          }
      }
      pc = next;
    }
    throw std::runtime_error("step limit");
  }
};
