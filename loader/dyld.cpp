#include "loader/dyld.h"

#include <algorithm>
#include <functional>

namespace radeki::loader {
namespace {

constexpr size_t kNone = static_cast<size_t>(-1);
constexpr int kMaxReexportDepth = 8;

std::string leafName(const std::string& p) {
  size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

}  // namespace

size_t Registry::indexOf(const std::string& path) const {
  if (path.empty()) return kNone;
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].path == path || (!entries_[i].installName.empty() && entries_[i].installName == path)) return i;
  std::string leaf = leafName(path);
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (leafName(entries_[i].path) == leaf) return i;
    if (!entries_[i].installName.empty() && leafName(entries_[i].installName) == leaf) return i;
  }
  return kNone;
}

int Registry::add(const Spec& spec) {
  if (spec.path.empty() || !spec.image) return -1;
  if (indexOf(spec.path) != kNone) return -1;
  if (spec.executable && executable()) return -1;
  Entry e;
  e.path = spec.path;
  e.image = spec.image;
  e.executable = spec.executable;
  e.global = spec.global;
  for (const auto& d : spec.image->dylibs) {
    if (d.kind == macho::DylibKind::Id) {
      if (e.installName.empty()) e.installName = d.name;
    } else {
      e.dependencies.push_back(d);
    }
  }
  entries_.push_back(std::move(e));
  return static_cast<int>(entries_.size() - 1);
}

const Entry* Registry::find(const std::string& path) const {
  size_t i = indexOf(path);
  return i == kNone ? nullptr : &entries_[i];
}

const Entry* Registry::executable() const {
  for (const auto& e : entries_)
    if (e.executable) return &e;
  return nullptr;
}

std::vector<Export> Registry::exportsOf(const std::string& path) const {
  size_t i = indexOf(path);
  return i == kNone ? std::vector<Export>{} : entries_[i].exports;
}

void Registry::collectExports(size_t index, uint64_t slide) {
  Entry& e = entries_[index];
  e.exports.clear();
  for (const auto& s : e.image->symbols) {
    if (s.undefined() || s.stab() || !s.external() || s.name.empty()) continue;
    e.exports.push_back({s.name, s.value + slide});
  }
}

std::optional<uint64_t> Registry::lookupIn(size_t index, const std::string& symbol, int depth) const {
  const Entry& e = entries_[index];
  if (!e.linked || depth > kMaxReexportDepth) return std::nullopt;
  for (const auto& x : e.exports)
    if (x.name == symbol) return x.addr;
  // LC_REEXPORT_DYLIB: the re-exporting image hands the symbol through unchanged.
  for (const auto& d : e.dependencies) {
    if (d.kind != macho::DylibKind::Reexport) continue;
    size_t j = indexOf(d.name);
    if (j == kNone || j == index) continue;
    if (auto v = lookupIn(j, symbol, depth + 1)) return v;
  }
  return std::nullopt;
}

// Symbol resolution during the relocation pass -- where Apple's system libraries are
// redirected onto the Android host. Order matters and mirrors dyld, then the compat layer:
//
//   1. Two-level namespace, guest side: the dylib named by the import's ordinal (install
//      names like "/usr/lib/libc++.1.dylib" match a registered image first), plus anything
//      that image re-exports (LC_REEXPORT_DYLIB, depth-limited, cycle-safe).
//   2. Flat namespace (ordinal -2, or -1/0 self, or flatFallback): every global loaded image.
//   3. Host runtime layer (opt_->fallback): the compat registry. This is where the IPA's
//      Apple-only imports land when no loaded image provides them:
//        - libSystem.B.dylib subset  -> identical host libc/libm/pthread functions;
//        - libc++abi.dylib           -> written C++ shims (atexit, guards, new/delete);
//        - libc++.1.dylib            -> the std::__1 forwarding table: explicit wrappers with
//                                       Apple's exact mangled names, backed by host code that
//                                       speaks Apple's ABI (e.g. basic_string::__init), plus a
//                                       dlsym pass-through onto the host's libc++ (libc++_shared
//                                       on Android uses the identical std::__1 mangling);
//        - UIKit/Foundation/etc.     -> hand-written shims (NSLog, UIApplicationMain, the objc
//                                       memory family) and dummy class objects for _OBJC_CLASS_$_.
//   4. Unresolved: strong imports become named BRK traps by default, or -- with
//      Options::stubFactory set -- host-executable logging no-op stubs. Weak imports bind null.
//
// Steps 1-2 can only answer from real guest code; steps 3-4 answer "Apple library requested,
// host provides" and are the only place host addresses ever enter the guest's bindings.
std::optional<uint64_t> Registry::resolve(const std::string& symbol, const std::string& dylib) const {
  if (!opt_) return std::nullopt;
  // 1. two-level namespace: only the named image (plus what it re-exports) is searched.
  if (!dylib.empty()) {
    size_t i = indexOf(dylib);
    if (i != kNone)
      if (auto v = lookupIn(i, symbol, 0)) return v;
  }
  // 2. flat lookup (dyld ordinal -2, or ordinal 0 "self"): every global image in link order.
  if (opt_->flatFallback)
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].global)
        if (auto v = lookupIn(i, symbol, 0)) return v;
  // 3. host runtime layer: compat libSystem + libc++ forwarding + framework shims/stubs.
  if (opt_->fallback) return opt_->fallback->resolve(symbol, dylib);
  // 4. nothing provided: the relinker applies its unresolved policy (trap stub or dispatch stub).
  return std::nullopt;
}

std::optional<compat::CompatEntry> Registry::describe(const std::string& symbol, const std::string& dylib) const {
  if (!opt_) return std::nullopt;
  if (!dylib.empty()) {
    size_t i = indexOf(dylib);
    if (i != kNone && lookupIn(i, symbol, 0))
      return compat::CompatEntry{compat::SymbolClass::IOSFramework, dylib, "image_export"};
  }
  if (opt_->flatFallback)
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].global && lookupIn(i, symbol, 0))
        return compat::CompatEntry{compat::SymbolClass::IOSFramework, entries_[i].path, "image_export"};
  if (opt_->fallback) return opt_->fallback->describe(symbol, dylib);
  return std::nullopt;
}

LoadResult Registry::linkAll(const Options& opt) {
  LoadResult res;
  opt_ = &opt;
  uint64_t pageSize = relinker::kPageSize;
  std::optional<bool> pointerWidth64;
  for (const auto& entry : entries_) {
    if (!entry.image) continue;
    const uint64_t imagePageSize = relinker::pageSizeForArch(entry.image->arch);
    if (!pointerWidth64) {
      pointerWidth64 = entry.image->is64;
      pageSize = imagePageSize;
    } else if (entry.image->is64 != *pointerWidth64 || imagePageSize != pageSize) {
      res.errors.push_back("loader cannot mix 32-bit and 64-bit guest images in one process");
      return res;
    }
  }
  if (opt.firstBase % pageSize || opt.spacing == 0 || opt.spacing % pageSize) {
    res.errors.push_back("loader options: firstBase and spacing must be non-zero and aligned to the guest page size");
    return res;
  }
  if (entries_.empty()) {
    res.warnings.push_back("no images registered");
    return res;
  }

  // Dependency-first ordering; registration order breaks ties; the executable goes last.
  std::vector<size_t> order;
  {
    std::vector<bool> done(entries_.size(), false);
    std::function<void(size_t)> visit = [&](size_t i) {
      if (done[i]) return;
      done[i] = true;  // set before recursing, so a dependency cycle cannot spin
      for (const auto& d : entries_[i].dependencies) {
        size_t j = indexOf(d.name);
        if (j != kNone && j != i) visit(j);
      }
      order.push_back(i);
    };
    for (size_t i = 0; i < entries_.size(); ++i)
      if (!entries_[i].executable) visit(i);
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].executable) visit(i);
  }

  // A second linkAll() on the same registry starts from a clean slate.
  for (auto& e : entries_) {
    e.linked = false;
    e.failed = false;
    e.error.clear();
    e.exports.clear();
    e.warnings.clear();
  }

  // Two passes. Pass 1 lays the images out and links them against whatever is already
  // prepared, which is what fixes every image's size. Pass 2 republishes the complete export
  // table (all slides are known by then) and links again, so an import can bind to an image
  // that is registered after its importer -- the case a flat-namespace lookup hits. Binding
  // does not change any image's size, so the pass-1 layout stays valid, and pass 2 can only
  // turn traps into real addresses, never the other way round.
  struct MappedRange { uint64_t begin, end; size_t owner; };
  std::vector<MappedRange> mappedRanges;
  auto runPass = [&](bool assignBases, bool collect) {
    uint64_t base = opt.firstBase;
    for (size_t idx : order) {
      Entry& e = entries_[idx];
      if (assignBases) {
        // Classic (pre-dyld-info) images carry no rebase records: dyld 1 loaded them at
        // their preferred vmaddr with slide 0, and every internal __DATA pointer is only
        // valid there. Honoring the preferred vmaddr is not a preference, it is the only
        // placement where the image works at all.
        e.loadBase = e.image->classicBinds ? e.image->textBase() : base;
      } else if (e.failed || e.image == nullptr) {
        // Already known to be unloadable (pass 1 saw it fail). Its error still has to reach
        // the report -- silently skipping it would make LoadResult::ok() lie.
        if (collect && !e.error.empty() && std::find(res.errors.begin(), res.errors.end(), e.error) == res.errors.end())
          res.errors.push_back(e.error);
        continue;
      }

      try {
        e.slide = e.loadBase - e.image->textBase();
      } catch (const FormatError& ex) {
        e.failed = true;
        e.linked = false;
        e.exports.clear();
        e.error = e.path + ": " + ex.what();
        if (collect && std::find(res.errors.begin(), res.errors.end(), e.error) == res.errors.end())
          res.errors.push_back(e.error);
        base += opt.spacing;
        continue;
      }
      // Exports are published before this image binds, so an import with the "self" ordinal
      // resolves inside its own image the way dyld resolves it.
      collectExports(idx, e.slide);
      e.linked = true;

      relinker::LinkOptions lo;
      lo.loadBase = e.loadBase;
      lo.resolver = this;
      lo.veneerSlots = opt.veneerSlots;
      lo.stubFactory = opt.stubFactory;
      try {
        e.linkedImage = relinker::link(*e.image, lo);
      } catch (const relinker::LinkError& ex) {
        e.linked = false;
        e.exports.clear();
        e.failed = true;
        e.error = e.path + ": link failed: " + ex.what();
        if (collect && std::find(res.errors.begin(), res.errors.end(), e.error) == res.errors.end())
          res.errors.push_back(e.error);
        base += opt.spacing;
        continue;
      }
      e.failed = false;
      e.error.clear();
      e.imageSize = e.linkedImage.imageSize;
      e.totalSize = e.linkedImage.totalSize;

      // Classic images are placed at fixed preferred addresses, so the moving base cursor
      // alone cannot prevent two images from mapping over one another. Reserve the complete
      // image-plus-stubs range on pass 1 and reject any collision before publishing it.
      if (assignBases) {
        uint64_t rangeEnd = 0;
        if (__builtin_add_overflow(e.loadBase, e.totalSize, &rangeEnd)) {
          e.linked = false;
          e.exports.clear();
          e.failed = true;
          e.error = e.path + ": mapped address range overflows";
          base += opt.spacing;
          continue;
        }
        auto collision = std::find_if(mappedRanges.begin(), mappedRanges.end(), [&](const MappedRange& r) {
          return e.loadBase < r.end && r.begin < rangeEnd;
        });
        if (collision != mappedRanges.end()) {
          e.linked = false;
          e.exports.clear();
          e.failed = true;
          e.error = e.path + ": mapped address range overlaps " + entries_[collision->owner].path;
          base += opt.spacing;
          continue;
        }
        mappedRanges.push_back({e.loadBase, rangeEnd, idx});
      }

      if (collect) {
        e.warnings.clear();
        for (const auto& w : e.linkedImage.warnings) e.warnings.push_back(w);
        // Report what could not be bound. Weak imports land here too, with trap == 0, because
        // dyld binds an unsatisfied weak import to null rather than failing.
        for (const auto& b : e.linkedImage.imports) {
          if (b.resolved) continue;
          bool dup = false;
          for (const auto& u : res.unresolved)
            if (u.path == e.path && u.symbol == b.symbol && u.dylib == b.dylib) { dup = true; break; }
          if (dup) continue;
          UnresolvedImport u;
          u.path = e.path;
          u.symbol = b.symbol;
          u.dylib = b.dylib;
          u.weak = b.weak;
          if (b.dispatchStub) u.stub = b.target;
          else
            for (const auto& t : e.linkedImage.traps)
              if (t.symbol == b.symbol) { u.trap = t.addr; break; }
          res.unresolved.push_back(std::move(u));
        }
        if (!e.executable && e.installName.empty())
          res.warnings.push_back(e.path + ": dylib has no LC_ID_DYLIB, so nothing can import from it by name");
      }
      // The cursor must clear the actual end of whatever was placed: fixed-placement classic
      // images can sit far below the moving base, and totalSize can exceed spacing.
      base = std::max(base + opt.spacing, alignUp(e.loadBase + e.totalSize, pageSize));
    }
  };

  runPass(/*assignBases=*/true, /*collect=*/false);
  res.unresolved.clear();
  runPass(/*assignBases=*/false, /*collect=*/true);

  if (opt.stubFactory) {
    size_t stubbed = 0;
    for (const auto& u : res.unresolved) stubbed += u.stub != 0;
    if (stubbed)
      res.warnings.push_back(std::to_string(stubbed) +
        " unresolved imports were bound to logging no-op dispatch stubs (they keep the guest "
        "running past the call; they are not implementations and stay listed as unresolved)");
  }

  res.order.clear();
  for (size_t idx : order)
    if (entries_[idx].image) res.order.push_back(&entries_[idx]);
  return res;
}

}  // namespace radeki::loader
