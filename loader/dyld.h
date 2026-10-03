// dyld-style image registry: holds several parsed Mach-O images (an executable plus the
// dylibs it imports), assigns each one a load base, links them in dependency order and binds
// imports across images instead of trapping them.
//
// Scope, stated plainly:
//  - This is a linker/loader for the images you hand it. It is not a replacement for dyld:
//    there is no dyld shared cache, no @rpath/@executable_path expansion against a real
//    filesystem, no initializer ordering across images, and no weak coalescing.
//  - Every image still goes through relinker::link, so unresolved strong imports keep
//    becoming named BRK trap stubs. Adding an image can only turn traps into real addresses;
//    it can never turn an unresolved symbol into a silent fake success.
//  - linkAll() runs two passes: the first fixes each image's load base and size, the second
//    binds against the complete export table. That is what lets a flat-namespace import bind
//    to an image registered after its importer, matching dyld, which binds only after every
//    image is mapped.
//  - macho::Image holds a non-owning view of the file bytes: the caller must keep both the
//    buffer and the Image alive for as long as the Registry uses them.
#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "mach_o/macho.h"
#include "relinker/relinker.h"

namespace radeki::loader {

struct Spec {
  std::string path;                    // install name for a dylib, path for the executable
  const macho::Image* image = nullptr; // non-owning
  bool executable = false;             // at most one
  bool global = true;                  // visible to flat (ordinal 0 / -2) lookups
};

struct Options {
  uint64_t firstBase = 0x300000000ull;  // must be 16K aligned
  uint64_t spacing = 0x4000000ull;      // gap between images, multiple of 16K
  uint32_t veneerSlots = 64;
  // Consulted after the loaded images: this is where the host-side runtime layer goes --
  // the compat libSystem subset, the libc++ -> Android libc++_shared forwarding table, and
  // the framework dummy objects. See Registry::resolve() for the full lookup order.
  const relinker::ImportResolver* fallback = nullptr;
  // When a two-level lookup finds nothing in the named dylib, also scan every loaded image.
  // dyld would not do this; it is here because our dylib set is usually incomplete.
  bool flatFallback = true;
  // Opt-in runtime mode: unresolved strong imports bind to host-executable logging stubs
  // instead of BRK trap stubs, so static constructors and the startup loop keep running.
  // Empty (default) keeps the honest traps. Either way, LoadResult::unresolved lists them.
  relinker::StubFactory stubFactory;
};

struct Export {
  std::string name;
  uint64_t addr = 0;  // slid
};

struct Entry {
  std::string path;
  const macho::Image* image = nullptr;
  bool executable = false, global = true;
  std::string installName;                  // LC_ID_DYLIB, empty for an executable
  std::vector<macho::Dylib> dependencies;   // non-ID dylib commands, in load order
  std::vector<Export> exports;              // slid; filled in when this image is linked
  uint64_t loadBase = 0, slide = 0, imageSize = 0, totalSize = 0;
  bool linked = false;   // exports published and relinker::link() succeeded
  bool failed = false;   // link failed; `error` says why and LoadResult::errors repeats it
  std::string error;     // "" unless failed
  relinker::LinkedImage linkedImage;
  std::vector<std::string> warnings;
};

struct UnresolvedImport {
  std::string path, symbol, dylib;
  bool weak = false;
  uint64_t trap = 0;  // address of the BRK stub; 0 for a weak import that bound to null
  uint64_t stub = 0;  // address of a dispatch stub (stub mode); trap == 0 then
};

struct LoadResult {
  std::vector<const Entry*> order;  // link order: dependencies before their importers
  std::vector<UnresolvedImport> unresolved;
  std::vector<std::string> warnings, errors;
  bool ok() const { return errors.empty(); }
};

class Registry : public relinker::ImportResolver {
 public:
  // Returns the entry index, or -1 when the path is already registered or the spec is invalid.
  int add(const Spec& spec);
  // Links every registered image. A per-image link failure is recorded in LoadResult::errors,
  // not thrown, so one bad dependency still leaves the rest of the report intact.
  LoadResult linkAll(const Options& opt);

  // relinker::ImportResolver, called from inside relinker::link while an image is bound.
  // `dylib` is the install name from the import's two-level ordinal, "" for flat lookups.
  std::optional<uint64_t> resolve(const std::string& symbol, const std::string& dylib) const override;
  std::optional<compat::CompatEntry> describe(const std::string& symbol, const std::string& dylib) const override;

  const Entry* find(const std::string& path) const;
  const Entry* executable() const;
  std::vector<Export> exportsOf(const std::string& path) const;
  size_t size() const { return entries_.size(); }

 private:
  std::optional<uint64_t> lookupIn(size_t index, const std::string& symbol, int depth) const;
  size_t indexOf(const std::string& path) const;
  void collectExports(size_t index, uint64_t slide);

  std::deque<Entry> entries_;   // deque: Entry addresses stay valid across add()
  const Options* opt_ = nullptr;
};

}  // namespace radeki::loader
