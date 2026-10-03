// Native ARM64 relinker: slides a Mach-O image to a new base, applies rebases/binds, resolves
// imports against a compatibility registry, and never fakes success for unresolved imports.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "mach_o/macho.h"
#include "compat/symbol_class.h"

namespace radeki::relinker {

struct LinkError : std::runtime_error { using std::runtime_error::runtime_error; };

constexpr uint64_t kPageSize = 0x4000;  // iOS 16K page; keeps ADRP page deltas valid

struct ImportResolver {
  virtual ~ImportResolver() = default;
  virtual std::optional<compat::CompatEntry> describe(const std::string&, const std::string&) const { return std::nullopt; }
  virtual std::optional<uint64_t> resolve(const std::string& symbol, const std::string& dylib) const = 0;
};

// When set, an unresolved STRONG import is bound to the host-executable stub the factory
// returns instead of a BRK trap (factory returning 0 falls back to the trap). The import is
// still reported as unresolved (BoundImport::dispatchStub, loader::UnresolvedImport::stub) --
// the stub keeps the guest running past the call, it never pretends the symbol is implemented.
using StubFactory = std::function<uint64_t(const std::string& symbol, const std::string& dylib)>;
class CompatRegistry : public ImportResolver {
 public:
  void add(const std::string& symbol, uint64_t address) { add(symbol, address, {compat::SymbolClass::CompatibilityShim, "libSystem", "compat_shim"}); }
  void add(const std::string& symbol, uint64_t address, compat::CompatEntry entry) { map_[symbol] = address; entries_[symbol] = std::move(entry); }
  size_t size() const { return map_.size(); }
  // Optional second-stage resolver consulted on a miss (e.g. the dlsym pass-through onto the
  // host libc++). Keeps the map authoritative: explicit entries always win.
  void chainNext(const ImportResolver* next) { next_ = next; }
  std::optional<compat::CompatEntry> describe(const std::string& s, const std::string& d) const override {
    auto it = entries_.find(s);
    if (it != entries_.end()) return it->second;
    return next_ ? next_->describe(s, d) : std::nullopt;
  }
  std::optional<uint64_t> resolve(const std::string& s, const std::string& d) const override {
    auto it = map_.find(s);
    if (it != map_.end()) return it->second;
    return next_ ? next_->resolve(s, d) : std::nullopt;
  }
 private:
  std::map<std::string, uint64_t> map_;
  std::map<std::string, compat::CompatEntry> entries_;
  const ImportResolver* next_ = nullptr;
};

struct LinkOptions {
  uint64_t loadBase = 0x200000000ull;        // must be 16K aligned
  const ImportResolver* resolver = nullptr;  // null => everything unresolved
  uint32_t veneerSlots = 64;
  uint64_t maxImageBytes = 1ull << 30;
  StubFactory stubFactory;                   // empty => keep honest BRK trap stubs (default)
};

struct Region { std::string name; uint64_t addr, size; uint32_t prot; bool code; };
// dispatchStub: the import still has no provider (resolved == false), but it was bound to a
// host-side logging stub instead of a BRK trap; `target` is that stub's host address.
struct BoundImport { std::string symbol, dylib; uint64_t slot; bool resolved, weak; uint64_t target; int trapIndex; compat::SymbolClass cls = compat::SymbolClass::Unsupported; std::string framework, method; bool dispatchStub = false; };
struct Trap { uint32_t index; uint64_t addr; std::string symbol; };

struct LinkedImage {
  uint64_t loadBase = 0, imageBase = 0, slide = 0, imageSize = 0, totalSize = 0;
  std::optional<uint64_t> entry;
  std::vector<uint8_t> memory;   // contiguous: image, then stub/veneer region
  std::vector<Region> regions;
  std::vector<BoundImport> imports;
  std::vector<Trap> traps;
  std::vector<std::pair<uint64_t, std::string>> definedSymbols;
  std::optional<std::string> symbolAt(uint64_t pc) const;
  std::optional<std::string> importAtTrap(uint64_t addr) const;       // BRK stubs standing in for unresolved strong imports
  std::vector<std::pair<uint64_t, uint32_t>> dataInCode;  // slid
  // Slid [start, end) ranges that actually hold instructions: sections carrying
  // S_ATTR_PURE_INSTRUCTIONS/S_ATTR_SOME_INSTRUCTIONS, plus the trap stubs and the veneers in
  // use. Executable segments with no such sections fall back to the whole segment. validate()
  // walks these, so Mach-O headers, load commands and string constants are not mistaken for
  // code just because they live in an executable segment.
  std::vector<std::pair<uint64_t, uint64_t>> codeRanges;
  std::vector<uint64_t> initPointerSlots;  // slid addresses of __mod_init_func entries
  uint64_t veneerBase = 0, veneerCount = 0, veneerCap = 0;
  uint64_t rebases = 0, binds = 0;
  std::vector<std::string> warnings;

  bool contains(uint64_t a, uint64_t n = 1) const { return a >= loadBase && a - loadBase + n <= totalSize && a - loadBase + n >= n; }
  uint32_t read32(uint64_t a) const;
  void write32(uint64_t a, uint32_t v);
  uint64_t read64(uint64_t a) const;
};

LinkedImage link(const macho::Image& img, const LinkOptions& opt);

// Redirect the B/BL at `site` to `target`; uses a veneer when out of direct range.
void patchBranch(LinkedImage& li, uint64_t site, uint64_t target, bool link);

struct Violation { uint64_t addr; std::string what; };
struct Validation { std::vector<Violation> violations, warnings; uint64_t checked = 0; bool ok() const { return violations.empty(); } };
// Verifies every PC-relative reference in executable regions stays inside the mapped image.
Validation validate(const LinkedImage& li);

}  // namespace radeki::relinker
