// Mach-O parsing: thin/fat, 32/64-bit, load commands, symbols, fixups.
// All input is untrusted; parse functions throw radeki::FormatError on malformed data.
// Image holds a NON-OWNING view of the slice bytes: keep the file buffer alive.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace radeki::macho {

enum class Arch { Unknown, ARMv6, ARMv7, ARMv7s, ARMv7k, ARM64, ARM64e, ARM64_32, X86_64, I386 };
const char* archName(Arch a);
Arch archFromCpu(uint32_t cputype, uint32_t cpusubtype);

enum : uint32_t {
  MH_EXECUTE = 2, MH_DYLIB = 6, MH_BUNDLE = 8,
  S_ZEROFILL = 0x1, S_MOD_INIT_FUNC_POINTERS = 0x9, S_MOD_TERM_FUNC_POINTERS = 0xA,
  S_GB_ZEROFILL = 0xC, S_THREAD_LOCAL_REGULAR = 0x11, S_THREAD_LOCAL_ZEROFILL = 0x12,
  S_THREAD_LOCAL_VARIABLES = 0x13, S_ATTR_PURE_INSTRUCTIONS = 0x80000000u,
  S_ATTR_SOME_INSTRUCTIONS = 0x00000400u,
  VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4,
};

struct SliceInfo {
  Arch arch = Arch::Unknown;
  uint32_t cputype = 0, cpusubtype = 0;
  uint64_t offset = 0, size = 0;
  bool fat = false;
  bool bigEndian = false;  // thin big-endian images are reported but unsupported
};

struct Section {
  std::string name, segment;
  uint64_t addr = 0, size = 0;
  uint32_t offset = 0, align = 0, reloff = 0, nreloc = 0, flags = 0, reserved1 = 0, reserved2 = 0;
  uint32_t type() const { return flags & 0xFF; }
  bool zerofill() const { auto t = type(); return t == S_ZEROFILL || t == S_GB_ZEROFILL || t == S_THREAD_LOCAL_ZEROFILL; }
  bool hasCode() const { return (flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0; }
};

struct Segment {
  std::string name;
  uint64_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
  uint32_t maxprot = 0, initprot = 0, flags = 0;
  std::vector<Section> sections;
};

struct Symbol {
  std::string name;
  uint8_t type = 0, sect = 0;
  uint16_t desc = 0;
  uint64_t value = 0;
  bool undefined() const { return (type & 0x0E) == 0; }
  bool external() const { return (type & 0x01) != 0; }
  bool stab() const { return (type & 0xE0) != 0; }
  int libOrdinal() const { return (desc >> 8) & 0xFF; }
};

enum class DylibKind { Load, Weak, Reexport, Lazy, Upward, Id };
struct Dylib {
  std::string name;
  DylibKind kind = DylibKind::Load;
  uint32_t currentVersion = 0, compatVersion = 0;
};

struct Import {
  std::string name;
  int libOrdinal = 0;  // 1-based; 0 = self, -1 = main executable, -2 = flat lookup, ...
  bool weak = false;
};

// Unified fixup record, produced from dyld-info opcodes or chained fixups.
struct Fixup {
  enum class Kind { Rebase, Bind } kind = Kind::Rebase;
  uint64_t addr = 0;        // unslid vmaddr of the 8-byte (or 4-byte) location
  uint64_t target = 0;      // Rebase: unslid target vmaddr
  uint8_t high8 = 0;        // Rebase: top byte to restore
  uint32_t importIndex = 0; // Bind: index into Image::imports
  int64_t addend = 0;       // Bind
  bool lazy = false, weakBind = false;
  bool auth = false;        // arm64e pointer authentication metadata present
  uint16_t diversity = 0;
  bool addrDiv = false;
  uint8_t key = 0;
};

struct Image {
  Bytes data;  // slice bytes (non-owning)
  Arch arch = Arch::Unknown;
  uint32_t cputype = 0, cpusubtype = 0, filetype = 0, flags = 0;
  bool is64 = false;
  std::vector<Segment> segments;
  std::vector<Symbol> symbols;
  std::vector<uint32_t> indirectSymbols;
  std::vector<Dylib> dylibs;
  std::vector<Import> imports;
  std::vector<Fixup> fixups;
  std::vector<uint64_t> functionStarts;                       // unslid vmaddrs
  std::vector<std::pair<uint64_t, uint32_t>> dataInCode;      // {unslid vmaddr, length}
  std::vector<std::string> rpaths;
  std::string uuid;
  std::optional<uint64_t> entry;                              // unslid vmaddr
  uint32_t buildPlatform = 0, minOs = 0, sdk = 0;
  bool hasChainedFixups = false, hasDyldInfo = false;
  uint32_t chainedPointerFormat = 0;
  bool hasExportsTrie = false;
  bool hasCodeSignature = false;
  uint32_t codeSigOff = 0, codeSigSize = 0;
  uint32_t cryptId = 0, cryptOff = 0, cryptSize = 0;           // FairPlay encryption info
  bool hasUnwindInfo = false, hasEhFrame = false, hasTLS = false;
  uint32_t initFuncSections = 0;

  uint64_t textBase() const;  // vmaddr of the segment that maps the Mach-O header
  uint64_t ptrSize() const { return is64 ? 8 : 4; }
  const Segment* findSegment(const std::string& n) const;
  const Section* findSection(const std::string& seg, const std::string& sect) const;
  bool hasSectionNamed(const std::string& sect) const;
  // Map an unslid vmaddr to a file offset within the slice (file-backed bytes only).
  std::optional<uint64_t> vmToFile(uint64_t vmaddr, uint64_t len = 1) const;
  bool isArm64e() const { return arch == Arch::ARM64e; }
};

// Container handling.
std::vector<SliceInfo> listSlices(Bytes file);  // throws on malformed fat header
Image parseSlice(Bytes file, const SliceInfo& slice);
Image parseFile(Bytes file);  // picks the preferred ARM64 slice, else ARMv7(s)

struct SliceChoice {
  std::optional<size_t> index;
  std::string reason;
};
// Architecture-neutral preference used by file inspection and CLI parsing.
SliceChoice chooseSlice(const std::vector<SliceInfo>& slices);
// Prefer the architecture executable in this process can natively run (ARMv7 in an AArch32
// process, ARM64 first elsewhere), while retaining a clear mismatch result for ARM-only files.
SliceChoice chooseSliceForHost(const std::vector<SliceInfo>& slices);

}  // namespace radeki::macho
