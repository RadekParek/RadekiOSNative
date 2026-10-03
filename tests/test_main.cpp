#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>

#include "arm64/arm64.h"
#include "binary_analysis/analysis.h"
#include "core/json.h"
#include "loader/dyld.h"
#include "mach_o/macho.h"
#include "mini_a64.h"
#include "objc/objc.h"
#include "relinker/relinker.h"
#include "runtime/runtime.h"
#include "runtime/compat_cxx.h"
#include "runtime/cxx_forward.h"
#include "runtime/stub_dispatch.h"
#include "runtime/framework_stubs.h"
#include "compat/matrix.h"
#include "ipa/inflate.h"
#include "ipa/png.h"
#include "ipa/plist.h"
#include "ipa/bundle.h"
#include "fixtures.h"
#include <filesystem>
#include <fstream>
#include "synth_macho.h"

using namespace radeki;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define THROWS(e, ex) do { ++checks; bool t = false; try { ex; } catch (const e&) { t = true; } if (!t) { ++failures; printf("FAIL %s:%d: expected %s\n", __FILE__, __LINE__, #e); } } while (0)
#define TEST(n) static void n()

TEST(bytes_bounds) {
  uint8_t d[4] = {1, 2, 3, 4};
  Reader r(Bytes(d, 4));
  CHECK(r.read<uint32_t>(0) == 0x04030201);
  THROWS(FormatError, r.read<uint32_t>(1));
  THROWS(FormatError, r.read<uint8_t>(UINT64_MAX));
  uint8_t u[3] = {0xE5, 0x8E, 0x26};
  Reader u2(Bytes(u, 3)); uint64_t o = 0;
  CHECK(u2.uleb(o, 3, "t") == 624485);
  uint8_t bad[2] = {0x80, 0x80}; Reader b2(Bytes(bad, 2)); o = 0;
  THROWS(FormatError, b2.uleb(o, 2, "t"));
  uint8_t sl[1] = {0x7F}; Reader s1(Bytes(sl, 1)); o = 0;
  CHECK(s1.sleb(o, 1, "t") == -1);
}

TEST(arm64_encodings) {
  using namespace a64;
  CHECK(decode(0x94000002).kind == Kind::BL && decode(0x94000002).imm == 8);
  CHECK(decode(0x17FFFFFF).kind == Kind::B && decode(0x17FFFFFF).imm == -4);
  CHECK(decode(0xD65F03C0).kind == Kind::RET);
  CHECK(decode(0xD63F0100).kind == Kind::BLR);
  CHECK(decode(0xD4200020).kind == Kind::BRK && decode(0xD4200020).brk == 1);
  CHECK(decode(0xD503233F).kind == Kind::PacSign);   // paciasp
  CHECK(decode(0xD50323BF).kind == Kind::PacAuth);   // autiasp
  CHECK(decode(0xD65F0BFF).kind == Kind::PacAuth);   // retaa
  CHECK(decode(0xD503201F).kind == Kind::Other);     // nop
  auto w = withTarget(0x90000000, 0x100001008, 0x100005000);
  CHECK(w && targetOf(decode(*w), 0x100001008) == 0x100005000);
  CHECK(!withTarget(0x90000000, 0, 1ull << 40));      // ADRP out of range
  CHECK(!encodeB(0, 1ull << 28, false));              // B out of range
  CHECK(!encodeB(0, 2, false));                       // misaligned
  auto adr = withTarget(0x10000001, 0x1000, 0x1ABC);
  CHECK(adr && targetOf(decode(*adr), 0x1000) == 0x1ABC && (*adr & 31) == 1);
  auto ldr = withTarget(0x58000003, 0x1000, 0x1100);
  CHECK(ldr && targetOf(decode(*ldr), 0x1000) == 0x1100);
}

TEST(fat_and_selection) {
  auto a = synth::build();
  auto f = synth::fat({{{0x0100000C, 0}, a}, {{12, 9}, a}});
  auto sl = macho::listSlices(f);
  CHECK(sl.size() == 2 && sl[0].arch == macho::Arch::ARM64 && sl[1].arch == macho::Arch::ARMv7);
  auto ch = macho::chooseSlice(sl);
  CHECK(ch.index && *ch.index == 0);
  auto only32 = macho::chooseSlice({sl[1]});
  CHECK(only32.index && *only32.index == 0);
  auto x86 = macho::chooseSlice({macho::SliceInfo{macho::Arch::X86_64, 0x01000007, 3, 0, 0, true, false}});
  CHECK(!x86.index && x86.reason.find("x86_64") != std::string::npos);
  auto fe = synth::fat({{{0x0100000C, 2}, a}});
  CHECK(macho::listSlices(fe)[0].arch == macho::Arch::ARM64e);
  fe[4 + 3] = 0;  // zero archs
  THROWS(FormatError, macho::listSlices(fe));
  uint8_t junk[16] = {0xCA, 0xFE, 0xBA, 0xBE, 0, 0, 0, 0x55};
  THROWS(FormatError, macho::listSlices(Bytes(junk, 16)));
}

TEST(parse_dyld_info) {
  auto f = synth::build();
  auto img = macho::parseFile(f);
  CHECK(img.arch == macho::Arch::ARM64 && img.is64 && img.segments.size() == 4);
  CHECK(img.entry && *img.entry == synth::kText);
  CHECK(img.dylibs.size() == 1 && img.dylibs[0].name == "/usr/lib/libSystem.B.dylib");
  CHECK(img.imports.size() == 2 && img.imports[0].name == "_puts" && img.imports[1].name == "_missing_fn");
  CHECK(img.fixups.size() == 3);
  size_t rebases = 0, binds = 0;
  for (auto& x : img.fixups) { (x.kind == macho::Fixup::Kind::Rebase ? rebases : binds)++; }
  CHECK(rebases == 1 && binds == 2);
  CHECK(img.functionStarts.size() == 2 && img.functionStarts[1] == synth::kText + 60);
  CHECK(img.symbols.size() == 4 && img.symbols[2].undefined());
  CHECK(img.uuid.rfind("A0A1A2A3", 0) == 0);
}

TEST(parse_chained) {
  for (int fmt : {2, 6}) {
    auto f = synth::build({fmt});
    auto img = macho::parseFile(f);
    CHECK(img.hasChainedFixups && img.fixups.size() == 3 && img.imports.size() == 2);
    CHECK(img.fixups[2].kind == macho::Fixup::Kind::Rebase && img.fixups[2].target == synth::kStr);
    CHECK(img.fixups[0].kind == macho::Fixup::Kind::Bind && img.imports[img.fixups[0].importIndex].name == "_puts");
    CHECK(img.fixups[1].addr == synth::kData + 8);
  }
  auto eb = synth::build({9, 2});
  auto e = macho::parseFile(eb);
  CHECK(e.arch == macho::Arch::ARM64e && e.fixups.size() == 3 && e.fixups[2].auth && e.fixups[2].diversity == 0x1234);
}

TEST(malformed_inputs) {
  auto good = synth::build();
  auto trunc = good; trunc.resize(40);
  THROWS(FormatError, macho::parseFile(trunc));
  auto m = good; m[16] = 0xFF; m[17] = 0xFF;  // absurd ncmds
  THROWS(FormatError, macho::parseFile(m));
  m = good; m[20] = 0xFF; m[21] = 0xFF; m[22] = 0xFF;  // sizeofcmds beyond file
  THROWS(FormatError, macho::parseFile(m));
  m = good; m[36] = 3;  // cmdsize of first command (PAGEZERO) -> misaligned/short
  THROWS(FormatError, macho::parseFile(m));
  m = good; m[0] = 0;
  THROWS(FormatError, macho::parseFile(m));
}

TEST(analysis_report) {
  auto f = synth::build();
  auto img = macho::parseFile(f);
  auto rep = analysis::analyze(img);
  CHECK(rep.instructions == 17 && rep.indirectBranches == 2 && rep.returns == 2);
  CHECK(rep.functions.size() == 2 && rep.functions[0].name == "_main" && rep.functions[1].name == "_helper");
  CHECK(rep.functions[0].size == 60 && rep.functions[1].size == 8);
  bool call = false, strRef = false;
  for (auto& x : rep.xrefs) {
    if (x.kind == analysis::XrefKind::Call && x.to == synth::kText + 60) call = true;
    if (x.kind == analysis::XrefKind::AdrpPair && x.to == synth::kStr) strRef = true;
  }
  CHECK(call && strRef);
  CHECK(rep.blockers.empty());
}

TEST(link_and_execute) {
  for (int fmt : {0, 2, 6}) {
    auto f = synth::build({fmt});
    auto img = macho::parseFile(f);
    relinker::CompatRegistry reg;
    reg.add("_puts", 0x300000000ull);
    relinker::LinkOptions opt;
    opt.loadBase = 0x240000000ull;  // different slide than the file's base
    opt.resolver = &reg;
    auto li = relinker::link(img, opt);
    CHECK(li.slide == 0x140000000ull && li.rebases == 1 && li.binds == 2);
    CHECK(li.read64(0x240004010) == 0x240002010);          // rebased pointer
    CHECK(li.read64(0x240004000) == 0x300000000ull);       // bound compat import
    CHECK(li.traps.size() == 1 && li.traps[0].symbol == "_missing_fn");
    CHECK(li.read64(0x240004008) == li.traps[0].addr);     // unresolved -> trap, not fake success
    CHECK(li.read32(li.traps[0].addr) == 0xD4200000);
    auto v = relinker::validate(li);
    CHECK(v.ok() && v.checked > 0);

    MiniCpu cpu; cpu.img = &li;
    cpu.hooks[0x300000000ull] = [](MiniCpu& c) { c.out += c.cstr(c.x[0]) + "\n"; c.x[0] = 1; };
    int rc = cpu.run(*li.entry);
    CHECK(rc == 7);
    CHECK(cpu.out == "hello radeki\nhello radeki\n");
    CHECK(cpu.trap.empty());
  }
}

TEST(unresolved_import_traps_with_symbol) {
  auto f = synth::build();
  auto img = macho::parseFile(f);
  auto li = relinker::link(img, {});  // no compat library at all
  CHECK(li.traps.size() == 2);
  MiniCpu cpu; cpu.img = &li;
  CHECK(cpu.run(*li.entry) == -1);
  CHECK(cpu.trap == "_puts");  // first call hits the BRK stub named after the symbol
}

TEST(link_refusals) {
  auto encb = synth::build({0, 0, 1});
  auto enc = macho::parseFile(encb);
  CHECK(enc.cryptId == 1);
  THROWS(relinker::LinkError, relinker::link(enc, {}));
  CHECK(!analysis::analyze(enc).blockers.empty());
  auto eb = synth::build({9, 2});
  auto e = macho::parseFile(eb);
  THROWS(relinker::LinkError, relinker::link(e, {}));
  auto f = synth::build(); auto img = macho::parseFile(f);
  relinker::LinkOptions o; o.loadBase = 0x1000;
  THROWS(relinker::LinkError, relinker::link(img, o));
  img.arch = macho::Arch::ARMv7;
  THROWS(relinker::LinkError, relinker::link(img, {}));
}

TEST(branch_veneer) {
  auto f = synth::build(); auto img = macho::parseFile(f);
  auto li = relinker::link(img, {});
  uint64_t site = li.loadBase + 0x1000 + 7 * 4;  // the bl helper
  uint64_t near = li.loadBase + 0x1000 + 15 * 4;
  relinker::patchBranch(li, site, near + 4, true);
  CHECK(li.veneerCount == 0 && a64::targetOf(a64::decode(li.read32(site)), site) == near + 4);
  uint64_t far = 0x7000000000ull;  // beyond +-128MB
  relinker::patchBranch(li, site, far, true);
  CHECK(li.veneerCount == 1);
  uint64_t vtarget = a64::targetOf(a64::decode(li.read32(site)), site);
  CHECK(vtarget == li.veneerBase && li.read64(vtarget + 8) == far);
  THROWS(relinker::LinkError, relinker::patchBranch(li, li.loadBase + 0x4000, far, false));  // data segment
  CHECK(relinker::validate(li).ok());
}

TEST(validator_catches_escape) {
  auto f = synth::build(); auto img = macho::parseFile(f);
  auto li = relinker::link(img, {});
  li.write32(li.loadBase + 0x1000, 0x94000000 | 0x02000000);  // bl -128MB: escapes the image
  auto v = relinker::validate(li);
  CHECK(!v.ok() && v.violations[0].addr == li.loadBase + 0x1000);
}

TEST(mutation_fuzz_no_crash) {
  std::mt19937 rng(12345);
  for (int fmt : {0, 6, 9}) {
    auto base = synth::build({fmt, fmt == 9 ? 2u : 0u});
    for (int it = 0; it < 4000; ++it) {
      auto m = base;
      int n = 1 + rng() % 4;
      for (int k = 0; k < n; ++k) m[(rng() % 3 == 0) ? 0x8000 + rng() % 0x180 : rng() % 0x400] = uint8_t(rng());
      try {
        auto img = macho::parseFile(m);
        analysis::analyze(img);
        relinker::LinkOptions o; o.maxImageBytes = 64ull << 20;
        auto li = relinker::link(img, o);
        relinker::validate(li);
      } catch (const FormatError&) {} catch (const relinker::LinkError&) {}
    }
  }
  // The same treatment for the metadata-bearing image. objc::parse documents that it must not
  // throw on hostile metadata, so an escaping FormatError is a failure, not an expected input.
  {
    auto base = synth::objcImage();
    int objcThrew = 0;
    for (int it = 0; it < 3000; ++it) {
      auto m = base;
      int n = 1 + rng() % 4;
      for (int k = 0; k < n; ++k) m[(rng() % 3 == 0) ? 0x4800 + rng() % 0x400 : rng() % 0xC200] = uint8_t(rng());
      try {
        auto img = macho::parseFile(m);
        try {
          objc::parse(img);
        } catch (const FormatError&) {
          ++objcThrew;
        }
        relinker::LinkOptions o; o.maxImageBytes = 64ull << 20;
        auto li = relinker::link(img, o);
        relinker::validate(li);
      } catch (const FormatError&) {} catch (const relinker::LinkError&) {}
    }
    CHECK(objcThrew == 0);
  }
  CHECK(true);
}

TEST(objc_metadata) {
  auto f = synth::objcImage();
  auto img = macho::parseFile(f);
  CHECK(img.arch == macho::Arch::ARM64 && std::string(macho::archName(img.arch)) == "arm64");
  CHECK(img.imports.size() == 1 && img.imports[0].name == "_missing_fn");
  CHECK(img.functionStarts.size() == 1 && img.functionStarts[0] == synth::kText);

  auto md = objc::parse(img);
  CHECK(md.present);
  CHECK(md.complete);  // the synthetic metadata is fully readable: no warnings at all
  CHECK(md.warnings.empty());
  CHECK(md.sections.size() == 5);
  CHECK(std::find(md.sections.begin(), md.sections.end(), "__objc_classlist") != md.sections.end());

  CHECK(md.classes.size() == 1);
  const auto& c = md.classes[0];
  CHECK(c.name == "Widget");
  CHECK(c.superName == "ModelBase");   // read through the superclass pointer, not guessed
  CHECK(c.instanceSize == 24);
  CHECK(c.methods.size() == 2 && c.methods[0].selector == "doWork" && c.methods[0].types == "v@:");
  CHECK(c.methods[1].selector == "init");
  CHECK(c.classMethods.size() == 1 && c.classMethods[0].selector == "shared" && c.classMethods[0].types == "@:");
  CHECK(c.methods[0].imp == synth::objcimg::kImp0);
  CHECK(c.ivars.size() == 1 && c.ivars[0].name == "counter" && c.ivars[0].type == "Q");
  CHECK(c.ivars[0].offset == 8 && c.ivars[0].size == 8);
  CHECK(c.properties.size() == 1 && c.properties[0].name == "name");
  CHECK(c.properties[0].attrs == "T@\"NSString\",&,N");

  CHECK(md.categories.size() == 1 && md.categories[0].name == "RadekiCat");
  CHECK(md.categories[0].className == "Widget");
  CHECK(md.categories[0].methods.size() == 1 && md.categories[0].methods[0].selector == "run");
  CHECK(md.categories[0].methods[0].types == "v@:");

  CHECK(md.protocols.size() == 1 && md.protocols[0].name == "RadekiProto");
  CHECK(md.protocols[0].methods.size() == 1 && md.protocols[0].methods[0].selector == "run");
  CHECK(c.protocols.size() == 1 && c.protocols[0] == "RadekiProto");  // class_ro_t baseProtocols

  CHECK(md.methodCount() == 5);
  CHECK(md.selectorCount() == 14);
  CHECK(std::find(md.selectors.begin(), md.selectors.end(), "doWork") != md.selectors.end());
  CHECK(std::find(md.selectors.begin(), md.selectors.end(), "RadekiCat") != md.selectors.end());
  CHECK(analysis::analyze(img).objc);
}

TEST(objc_metadata_hostile_inputs) {
  auto base = synth::objcImage();

  // A tagged/PAC pointer in class_data_bits_t: reported, not guessed at, and never a crash.
  {
    auto m = base;
    synth::p64(m, 0x4820, 0x1);  // class_data_bits_t of the Widget class object
    auto img = macho::parseFile(m);
    auto md = objc::parse(img);
    CHECK(md.present && !md.complete);
    CHECK(md.classes.size() == 1 && md.classes[0].name == "<unreadable>");
    CHECK(!md.warnings.empty());
  }
  // An absurd method-list count: the list is refused with a warning instead of being walked.
  {
    auto m = base;
    synth::p32(m, 0x4000 + synth::objcimg::kWidgetMethods + 4, 0xFFFFFFFFu);
    auto img = macho::parseFile(m);
    auto md = objc::parse(img);
    CHECK(!md.complete && md.classes.size() == 1 && md.classes[0].methods.empty());
  }
  // An unknown method entry size: neither encoding is assumed.
  {
    auto m = base;
    synth::p32(m, 0x4000 + synth::objcimg::kWidgetMethods, 99);
    auto img = macho::parseFile(m);
    auto md = objc::parse(img);
    CHECK(!md.complete && md.classes[0].methods.empty());
  }
  // A class object with no mapped memory behind it at all.
  {
    auto m = base;
    synth::p64(m, 0x4000 + synth::objcimg::kClassList, 0x7000000000ull);  // far outside the image
    auto img = macho::parseFile(m);
    auto md = objc::parse(img);
    CHECK(!md.complete && md.warnings.size() >= 1);
  }
}

TEST(objc_metadata_after_link) {
  auto f = synth::objcImage();
  auto img = macho::parseFile(f);
  relinker::CompatRegistry reg;
  reg.add("_puts", 0x300000000ull);
  relinker::LinkOptions o;
  o.loadBase = 0x240000000ull;
  o.resolver = &reg;
  auto li = relinker::link(img, o);
  CHECK(li.binds == 1 && li.traps.size() == 1 && li.traps[0].symbol == "_missing_fn");
  CHECK(relinker::validate(li).ok());

  const uint64_t D = synth::kData, s = li.slide;
  auto at = [&](uint32_t dataOff) { return D + dataOff + s; };
  // Every metadata pointer was rebased, so the class object still leads to its ro and to its
  // superclass after the slide.
  CHECK(li.read64(at(synth::objcimg::kWidgetClass) + 0x20) == at(synth::objcimg::kWidgetRo));
  CHECK(li.read64(at(synth::objcimg::kWidgetClass) + 0x08) == at(synth::objcimg::kBaseClass));
  CHECK(li.read64(at(synth::objcimg::kWidgetRo) + 32) == at(synth::objcimg::kWidgetMethods));
  CHECK(li.read64(at(synth::objcimg::kWidgetRo) + 40) == at(synth::objcimg::kProtocolList));
  CHECK(li.read64(at(synth::objcimg::kProtocolList) + 8) == at(synth::objcimg::kProtocol));
  CHECK(li.read64(at(synth::objcimg::kWidgetMethods + 8 + 16)) == synth::objcimg::kImp0 + s);
  // The relative method entry stayed relative -- it must not have been rebased.
  uint32_t rel = li.read32(at(synth::objcimg::kWidgetClassMethods) + 8);
  uint64_t nameAddr = at(synth::objcimg::kWidgetClassMethods) + 8 + uint64_t(int64_t(int32_t(rel)));
  CHECK(nameAddr == at(synth::objcimg::kMethnameSec + synth::objcimg::sShared));
}

TEST(dyld_binds_across_images) {
  auto exeBytes = synth::build();
  auto dylibBytes = synth::dylib();
  auto exe = macho::parseFile(exeBytes);
  auto dyl = macho::parseFile(dylibBytes);
  CHECK(dyl.filetype == 6 && !dyl.entry);
  CHECK(dyl.dylibs.size() == 1 && dyl.dylibs[0].name == "/usr/lib/libSystem.B.dylib");

  loader::Registry reg;
  CHECK(reg.add({"radeki", &exe, true}) == 0);
  CHECK(reg.add({"libSystem", &dyl, false}) == 1);
  CHECK(reg.add({"libSystem", &dyl, false}) == -1);   // the same path twice is refused
  CHECK(reg.add({"other", &exe, true}) == -1);        // only one executable
  CHECK(reg.add({"", &dyl, false}) == -1);            // no path, no image
  CHECK(reg.add({"null", nullptr, false}) == -1);
  CHECK(reg.size() == 2);

  auto compat = runtime::makeCompatRegistry();
  loader::Options o;
  o.fallback = &compat;
  auto lr = reg.linkAll(o);
  CHECK(lr.ok());
  CHECK(lr.errors.empty());
  CHECK(lr.order.size() == 2);
  CHECK(lr.order[0]->path == "libSystem" && lr.order[1]->path == "radeki");  // dependency first
  CHECK(lr.unresolved.empty());  // _puts via the compat registry, _missing_fn via the dylib

  const loader::Entry* e = reg.find("radeki");
  const loader::Entry* d = reg.find("/usr/lib/libSystem.B.dylib");  // install name also matches
  CHECK(e && d && e->linked && d->linked);
  CHECK(e->loadBase == o.firstBase + 0x4000000 && d->loadBase == o.firstBase);
  CHECK(e->slide == e->loadBase - synth::kBase);
  CHECK(e->linkedImage.traps.empty());  // nothing left unbound
  CHECK(relinker::validate(e->linkedImage).ok());

  // The import slot that used to hold a trap stub now points inside the other image.
  uint64_t slot = e->linkedImage.read64(e->loadBase + 0x4008);
  CHECK(slot == d->loadBase + 0x1000);
  CHECK(slot >= d->loadBase && slot < d->loadBase + d->linkedImage.totalSize);

  auto ex = reg.exportsOf("libSystem");
  bool missing = false;
  for (auto& x : ex)
    if (x.name == "_missing_fn") { missing = true; CHECK(x.addr == d->loadBase + 0x1000); }
  CHECK(missing);
  CHECK(reg.exportsOf("nope").empty());
  CHECK(reg.resolve("_puts", "") != std::nullopt);              // compat fallback
  CHECK(reg.resolve("_missing_fn", "libSystem") != std::nullopt);
  CHECK(reg.resolve("_UIApplicationMain", "") == std::nullopt);
  CHECK(reg.executable() == e);
}

TEST(dyld_flat_namespace_late_provider) {
  // The provider is registered *after* its importer and is not named by the importer's dylib
  // list, which is exactly the flat-namespace case: it only binds because linkAll() republishes
  // the complete export table in its second pass.
  auto exeBytes = synth::build({0, 0, 0, true});
  auto dylibBytes = synth::dylib();
  auto exe = macho::parseFile(exeBytes);
  auto dyl = macho::parseFile(dylibBytes);
  CHECK(exe.imports.size() == 3 && exe.imports[2].name == "_flat_sym" && exe.imports[2].libOrdinal == -2);

  loader::Registry reg;
  CHECK(reg.add({"radeki", &exe, true}) == 0);
  CHECK(reg.add({"libSystem", &dyl, false}) == 1);
  auto compat = runtime::makeCompatRegistry();
  loader::Options o;
  o.fallback = &compat;
  auto lr = reg.linkAll(o);
  CHECK(lr.ok() && lr.unresolved.empty());
  const loader::Entry* e = reg.find("radeki");
  const loader::Entry* d = reg.find("libSystem");
  CHECK(e && d);
  CHECK(e->linkedImage.traps.empty());
  CHECK(e->linkedImage.read64(e->loadBase + 0x4018) == d->loadBase + 0x1008);  // _flat_sym
}

TEST(dyld_keeps_unresolved_imports_honest) {
  auto exeBytes = synth::build({0, 0, 0, true});
  auto exe = macho::parseFile(exeBytes);
  loader::Registry reg;
  CHECK(reg.add({"radeki", &exe, true}) == 0);
  auto compat = runtime::makeCompatRegistry();
  loader::Options o;
  o.fallback = &compat;
  auto lr = reg.linkAll(o);
  CHECK(lr.ok());                       // nothing failed to link...
  CHECK(lr.unresolved.size() == 2);     // ...but two imports have no provider
  bool missing = false, flat = false, putsTrap = false;
  for (auto& u : lr.unresolved) {
    if (u.symbol == "_missing_fn") { missing = true; CHECK(u.trap != 0); }
    if (u.symbol == "_flat_sym") flat = true;
    if (u.symbol == "_puts") putsTrap = true;
  }
  CHECK(missing && flat && !putsTrap);  // _puts came from the compat registry
  const loader::Entry* e = reg.find("radeki");
  CHECK(e->linkedImage.traps.size() == 2);          // named BRK stubs, not fake success
  CHECK(e->linkedImage.read64(e->loadBase + 0x4008) ==
        e->linkedImage.read64(e->loadBase + 0x4008));  // stable read
  bool hasMissingTrap = false;
  for (auto& t : e->linkedImage.traps)
    if (t.symbol == "_missing_fn") hasMissingTrap = true;
  CHECK(hasMissingTrap);
}

TEST(dyld_error_reporting) {
  auto exeBytes = synth::build();
  auto exe = macho::parseFile(exeBytes);
  auto arm64eBytes = synth::build({9, 2});
  auto arm64e = macho::parseFile(arm64eBytes);
  auto compat = runtime::makeCompatRegistry();

  {  // an unaligned base is refused up front instead of producing a broken layout
    loader::Registry reg;
    reg.add({"radeki", &exe, true});
    loader::Options o;
    o.firstBase = 0x1234;
    auto lr = reg.linkAll(o);
    CHECK(!lr.ok() && lr.errors.size() == 1);
  }
  {  // an empty registry reports that, rather than pretending to load
    loader::Registry reg;
    loader::Options o;
    auto lr = reg.linkAll(o);
    CHECK(lr.ok() && lr.order.empty() && lr.warnings.size() == 1);
  }
  {  // one bad dependency must not take the rest of the report down with it
    loader::Registry reg;
    reg.add({"radeki", &exe, true});
    reg.add({"pac", &arm64e, false});
    loader::Options o;
    o.fallback = &compat;
    auto lr = reg.linkAll(o);
    CHECK(!lr.ok() && lr.errors.size() == 1);
    CHECK(lr.errors[0].find("pac: link failed") == 0);
    CHECK(lr.errors[0].find("PAC") != std::string::npos);
    CHECK(reg.find("pac")->failed && !reg.find("pac")->linked);
    CHECK(reg.find("pac")->exports.empty());   // a failed image cannot satisfy an import
    CHECK(reg.find("radeki")->linked);         // the good image still linked
    CHECK(lr.unresolved.size() == 1 && lr.unresolved[0].symbol == "_missing_fn");
  }
}

TEST(dyld_run_loaded_images) {
  auto exeBytes = synth::build();
  auto dylibBytes = synth::dylib();
  auto exe = macho::parseFile(exeBytes);
  auto dyl = macho::parseFile(dylibBytes);
  loader::Registry reg;
  reg.add({"radeki", &exe, true});
  reg.add({"libSystem", &dyl, false});
  auto compat = runtime::makeCompatRegistry();
  loader::Options o;
  o.fallback = &compat;
  auto mr = runtime::runLoadedImage(reg, o);
  CHECK(mr.images.size() == 2);
  CHECK(mr.images[0].path == "libSystem");
  CHECK(mr.images[0].skipped);
#if defined(__aarch64__)
  CHECK(mr.images[0].error.find("no entry point") != std::string::npos);  // a dylib has no entry
  CHECK(mr.ran && mr.exitCode == 7 && mr.output == "hello radeki\nhello radeki\n");
  CHECK(!mr.crashed && mr.ok());
  CHECK(mr.images[1].ran && mr.images[1].exitCode == 7);
#else
  // Honest on hosts that cannot execute AArch64 code: no pretending, just the reason.
  CHECK(!mr.ran && !mr.ok() && mr.error.find("ARM64 host") != std::string::npos);
  CHECK(mr.images[1].skipped && mr.images[1].error.find("ARM64 host") != std::string::npos);
#endif
}

TEST(compat_and_runtime) {
  auto reg = runtime::makeCompatRegistry();
  CHECK(reg.resolve("_puts", "") && reg.resolve("_malloc", "") && reg.resolve("___stack_chk_guard", ""));
  CHECK(!reg.resolve("_UIApplicationMain", ""));
  using PutsFn = int (*)(const char*);
  auto puts = reinterpret_cast<PutsFn>(*reg.resolve("_puts", ""));
  runtime::takeGuestOutput();
  puts("hi");
  CHECK(runtime::takeGuestOutput() == "hi\n");
  auto f = synth::build(); auto img = macho::parseFile(f);
  auto r = runtime::runImage(img);
#if defined(__aarch64__)
  CHECK(r.ran && r.exitCode == 7 && r.output == "hello radeki\nhello radeki\n");
#else
  CHECK(!r.ran && r.error.find("ARM64 host") != std::string::npos);  // honest on non-ARM64 hosts
#endif
  auto tb = synth::build({0, 0, 0}); auto timg = macho::parseFile(tb); timg.hasTLS = true;
  CHECK(runtime::runImage(timg).error.find("thread-local") != std::string::npos);
}

TEST(json_writer) {
  JsonWriter j;
  j.beginObject().kv("a", "x\"y").key("l").beginArray().num(1).num(-2).endArray().kvh("h", 255).kvb("b", true).endObject();
  CHECK(j.str() == R"({"a":"x\"y","l":[1,-2],"h":"0xff","b":true})");
}

static std::vector<int> exitOrder;
static void recordExit(void* arg) { exitOrder.push_back(static_cast<int>(reinterpret_cast<intptr_t>(arg))); }
TEST(batch2_status_and_cxx) {
  using namespace compat;
  CHECK(capabilityMatrix().size() == 21);
  Detected d;
  d.objc = d.gles = true; d.unresolvedStrong = 492;
  CHECK(stateFor(d) == RuntimeState::Analyzed);
  d.linked = true;
  CHECK(stateFor(d) == RuntimeState::PartiallyRelinked);
  CHECK(summaryFor(d, 492).find("0 of 492") != std::string::npos);
  d.unresolvedStrong = 0;
  CHECK(stateFor(d) == RuntimeState::Relinked);
  d.gles = d.objc = false;
  CHECK(stateFor(d) == RuntimeState::RuntimePartial); // C++ exceptions not supported
  d.blockedReason = "encrypted";
  CHECK(stateFor(d) == RuntimeState::Blocked);
  CHECK(std::string(runtimeStateName(stateFor(d))) == "BLOCKED");
  int a = 0, b = 0;
  CHECK(runtime::registerCxxExit(recordExit, reinterpret_cast<void*>(1), &a) == 0);
  CHECK(runtime::registerCxxExit(recordExit, reinterpret_cast<void*>(2), &b) == 0);
  CHECK(runtime::registerCxxExit(recordExit, reinterpret_cast<void*>(3), &a) == 0);
  runtime::finalizeCxx(&a);
  CHECK(exitOrder == std::vector<int>({3,1}));
  runtime::finalizeCxx(&a);
  CHECK(exitOrder.size() == 2);
  runtime::finalizeCxx(nullptr);
  CHECK(exitOrder == std::vector<int>({3,1,2}));
  uint64_t guard = 0;
  CHECK(runtime::acquireCxxGuard(&guard) == 1);
  runtime::abortCxxGuard(&guard);
  CHECK(runtime::acquireCxxGuard(&guard) == 1);
  runtime::releaseCxxGuard(&guard);
  CHECK(runtime::acquireCxxGuard(&guard) == 0);
  auto bytes=synth::build();auto img=macho::parseFile(bytes);
  auto weakImg=img;
  weakImg.imports[1].weak=true;
  auto weak=relinker::link(weakImg,{});
  CHECK(weak.imports.size()==2);
  CHECK(weak.imports[1].cls==SymbolClass::WeakOptional && weak.imports[1].method=="weak_null");
  CHECK(weak.read64(weak.imports[1].slot)==0 && weak.imports[1].target==0);
  auto linked=relinker::link(img,{});
  CHECK(linked.imports.size()==2);
  for(const auto& imp:linked.imports){
    CHECK(imp.cls==SymbolClass::Unsupported && imp.method=="trap");
    CHECK(linked.importAtTrap(imp.target)==imp.symbol);
  }
  auto reg = runtime::makeCompatRegistry();
  CHECK(reg.resolve("_exit", "") != reg.resolve("__exit", ""));
  CHECK(reg.resolve("___cxa_atexit", "").has_value());
  CHECK(!reg.resolve("___cxa_throw", "").has_value());
  auto entry = reg.describe("___cxa_atexit", "");
  CHECK(entry && entry->cls == SymbolClass::CompatibilityShim && entry->framework == "libc++abi");
  relinker::LinkedImage li;
  li.definedSymbols = {{0x1000, "_main"}, {0x2000, "_next"}};
  CHECK(li.symbolAt(0x1004) == "_main");
  CHECK(!li.symbolAt(0x12000));
  li.traps = {{0, 0x3000, "_missing"}};
  CHECK(li.importAtTrap(0x3000) == "_missing");
  CHECK(!li.importAtTrap(0x3004));
  runtime::RunResult crash;crash.crashed=true;crash.signalName="SIGTRAP";
  crash.trapSymbol="_not_supported";crash.subsystem="UIKit";crash.resolutionMethod="trap";
  crash.faultPc=0x3000;crash.callAddress=0x1000;crash.callSymbol="_main";
  crash.image="/data/app/game";crash.threadId=1234;
  std::string description=runtime::describeResult(crash);
  for(const auto& fragment : {"SIGTRAP","_not_supported","UIKit","trap","0x3000","_main","/data/app/game","1234"})
    CHECK(description.find(fragment)!=std::string::npos);
  runtime::resetRecentCompatCalls();
  for(int i=0;i<40;++i)runtime::noteCompatCall("call"+std::to_string(i));
  auto calls=runtime::recentCompatCalls();
  CHECK(calls.size()==32 && calls.front()=="call8" && calls.back()=="call39");
  runtime::resetRecentCompatCalls();
  runtime::noteCompatCall("literal-compat-name");
  CHECK(runtime::recentCompatCalls() == std::vector<std::string>{"literal-compat-name"});
  runtime::resetRecentCompatCalls();
  runtime::noteCompatCall(std::string(200, 'x'));
  auto truncatedCall=runtime::recentCompatCalls();
  CHECK(truncatedCall.size()==1 && truncatedCall[0]==std::string(127,'x'));
  runtime::resetRecentCompatCalls();CHECK(runtime::recentCompatCalls().empty());
  Detected app;app.linked=true;app.uikit=true;CHECK(stateFor(app)==RuntimeState::Relinked);
  CHECK(summaryFor(app).find("unsupported: UIKit")!=std::string::npos);
  for(const auto& row:capabilityMatrix())CHECK(!row.title.empty() && !row.detail.empty() && !row.subsystem.empty());
}

TEST(batch2_loader_classification) {
  auto exeBytes=synth::build(),libBytes=synth::dylib();
  auto exe=macho::parseFile(exeBytes),lib=macho::parseFile(libBytes);
  loader::Registry registry;
  CHECK(registry.add({"game",&exe,true})==0);
  CHECK(registry.add({"libSystem",&lib,false})==1);
  auto compat=runtime::makeCompatRegistry();loader::Options opt;opt.fallback=&compat;
  auto result=registry.linkAll(opt);
  CHECK(result.ok());
  auto exported=registry.describe("_missing_fn","/usr/lib/libSystem.B.dylib");
  CHECK(exported && exported->cls==compat::SymbolClass::IOSFramework && exported->method=="image_export");
  auto shim=registry.describe("_puts","/usr/lib/libSystem.B.dylib");
  CHECK(shim && shim->cls==compat::SymbolClass::CompatibilityShim);
  auto* game=registry.find("game");
  CHECK(game && game->linked);
  if(game){
    for(const auto& bound:game->linkedImage.imports)
      CHECK(bound.resolved && bound.cls!=compat::SymbolClass::Unsupported && !bound.method.empty());
  }
}

TEST(batch2_ipa) {
  using namespace ipa;
  std::string error;std::vector<uint8_t> output;
  CHECK(inflateRaw(fixtures::fixed,output,8192,error) && output==fixtures::fixedExpected);
  for(size_t i=0;i<fixtures::fixed.size();++i){
    std::vector<uint8_t> prefix(fixtures::fixed.begin(),fixtures::fixed.begin()+i);
    CHECK(!inflateRaw(prefix,output,8192,error));
  }
  CHECK(inflateRaw(fixtures::dynamic,output,8192,error) && output==fixtures::dynamicExpected);
  CHECK(!inflateRaw(fixtures::fixed,output,100,error) && !error.empty());
  auto invalid=fixtures::fixed;invalid.push_back(2);
  CHECK(!inflateRaw(invalid,output,8192,error));
  auto z=zlibDeflateStored(fixtures::fixedExpected);
  CHECK(zlibInflate(z,output,8192,error) && output==fixtures::fixedExpected);
  z.back() ^= 1;
  CHECK(!zlibInflate(z,output,8192,error));
  RgbaImage pixel;
  CHECK(decodePng(fixtures::cgbi,pixel,error));
  CHECK(pixel.width==1 && pixel.height==1 && pixel.rgba.size()==4);
  CHECK(pixel.rgba[0]==50 && pixel.rgba[1]==24 && pixel.rgba[2]==12 && pixel.rgba[3]==128);
  auto standard=encodePng(pixel);
  RgbaImage again;CHECK(decodePng(standard,again,error) && again.rgba==pixel.rgba);
  PngInfo meta;CHECK(pngInfo(standard,meta,error) && !meta.cgbi);
  Plist tree;
  CHECK(parsePlist(fixtures::binaryPlist,tree,error));
  CHECK(tree.path("CFBundleDisplayName") && tree.path("CFBundleDisplayName")->stringOr()=="Example & Friends");
  CHECK(tree.path("Items") && tree.path("Items")->stringsOr().size()==2);
  CHECK(tree.path("Unicode") && tree.path("Unicode")->stringOr()=="🚀");
  CHECK(parsePlist(fixtures::xmlPlist,tree,error));
  CHECK(tree.path("Version") && tree.path("Version")->intOr()==17);
  auto bad=fixtures::binaryPlist;bad.resize(bad.size()-6);
  CHECK(!parsePlist(bad,tree,error) && !error.empty());
  bad={'<','p','l','i','s','t','>','<','t','r','u','e','/','>'};
  CHECK(!parsePlist(bad,tree,error));
  namespace fs=std::filesystem;
  fs::path root=fs::temp_directory_path()/"radeki-batch2-fixture.app";
  fs::remove_all(root);fs::create_directories(root);
  auto write=[&](const fs::path& p,const std::vector<uint8_t>& data){std::ofstream f(p,std::ios::binary);f.write((const char*)data.data(),data.size());};
  write(root/"Info.plist",fixtures::xmlPlist);
  std::ofstream(root/"Demo") << "not a Mach-O";
  write(root/"DemoIcon.png",standard);
  RgbaImage decoy;decoy.width=20;decoy.height=2;decoy.rgba.assign(160,255);
  write(root/"appicon_decoy.png",encodePng(decoy));
  write(root/"DemoIcon@2x.png",standard);
  write(root/"DemoIcon.png",encodePng(decoy));
  BundleInfo info;fs::path icon=root/"saved"/"icon.png";
  CHECK(inspectBundle(root.string(),icon.string(),info,error) && info.iconPath==icon.string());
  CHECK(info.bundleIdentifier=="org.example.demo" && info.name=="Example & Friends");
  { std::ifstream f(icon,std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f),{}};
    CHECK(decodePng(bytes,again,error) && again.rgba==pixel.rgba); }
  fs::remove(root/"DemoIcon.png");fs::remove(root/"DemoIcon@2x.png");
  fs::remove(root/"appicon_decoy.png");write(root/"Assets.car",fixtures::cgbi);
  CHECK(inspectBundle(root.string(),icon.string(),info,error) && info.iconPath==icon.string());
  fs::remove(root/"Assets.car");
  CHECK(inspectBundle(root.string(),icon.string(),info,error) && info.iconPath.empty() && !info.warnings.empty());
  CHECK(inspectBundle(root.string()+"/",icon.string(),info,error) && info.bundleIdentifier=="org.example.demo");
  fs::path relocated=fs::temp_directory_path()/"radeki-batch2-relocated-bundle";
  fs::remove_all(relocated);
  fs::rename(root,relocated);
  CHECK(inspectBundle(relocated.string(),(relocated/"icon.png").string(),info,error) && info.bundleIdentifier=="org.example.demo");
  fs::remove_all(relocated);
}

// --- batch 3: libc++ forwarding, framework stubs, dispatch-stub linker mode ----------------

TEST(cxx_forward_string_wrappers) {
  using namespace compat;
  auto reg = runtime::makeCompatRegistry();
  const std::string S = "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
  // The exact symbol from the MCPE halt log resolves through the compat layer...
  CHECK(reg.resolve(S + "6__initEPKcm", "/usr/lib/libc++.1.dylib").has_value());
  auto entry = reg.describe(S + "6__initEPKcm", "/usr/lib/libc++.1.dylib");
  CHECK(entry && entry->cls == SymbolClass::AndroidBackend && entry->framework == "libc++");
  CHECK(entry->method.find("__init") != std::string::npos);
  // ...while exception machinery deliberately stays unbound (it would trap, or stub-log).
  CHECK(!reg.resolve("___cxa_throw", "/usr/lib/libc++abi.dylib").has_value());
  CHECK(runtime::cxxForwardTable().size() >= 20);
  CHECK(runtime::lookupCxxForward(S + "D1Ev").has_value());
  CHECK(!runtime::lookupCxxForward("__ZNSt3__1not_a_symbol").has_value());

  // The MCPE crash reached libc++'s __hash_table::__insert_unique and called this import.
  // It must return the next usable bucket prime instead of falling through to a zero stub.
  const std::string nextPrimeSymbol = "__ZNSt3__112__next_primeEm";
  auto nextPrimeEntry = reg.describe(nextPrimeSymbol, "/usr/lib/libc++.1.dylib");
  CHECK(nextPrimeEntry && nextPrimeEntry->cls == SymbolClass::AndroidBackend);
  CHECK(runtime::lookupCxxForward(nextPrimeSymbol).has_value());
  using NextPrimeFn = size_t (*)(size_t);
  auto nextPrime = reinterpret_cast<NextPrimeFn>(*reg.resolve(nextPrimeSymbol, ""));
  CHECK(nextPrime(0) == 0 && nextPrime(1) == 2 && nextPrime(2) == 2);
  CHECK(nextPrime(3) == 3 && nextPrime(4) == 5 && nextPrime(17) == 17);
  CHECK(nextPrime(18) == 19 && nextPrime(100) == 101 && nextPrime(1000) == 1009);
  bool validSmallPrimes = true;
  for (size_t n = 1; n < 512; ++n) {
    size_t p = nextPrime(n);
    if (p < n || p < 2) { validSmallPrimes = false; break; }
    for (size_t d = 2; d <= p / d; ++d) {
      if (p % d == 0) { validSmallPrimes = false; break; }
    }
    if (!validSmallPrimes) break;
  }
  CHECK(validSmallPrimes);
#if SIZE_MAX > UINT32_MAX
  // Exercise the deterministic 64-bit primality path (largest prime below SIZE_MAX).
  CHECK(nextPrime(static_cast<size_t>(18446744073709551555ull)) ==
        static_cast<size_t>(18446744073709551557ull));
#endif

  using InitFn = void (*)(void*, const char*, uint64_t);
  using VoidFn = void (*)(void*);
  auto init = reinterpret_cast<InitFn>(*reg.resolve(S + "6__initEPKcm", ""));
  auto dtor = reinterpret_cast<VoidFn>(*reg.resolve(S + "D1Ev", ""));

  // Short string: SSO inline, size<<1 in byte 0, LSB clear.
  alignas(8) uint8_t buf[24];
  init(buf, "hi", 2);
  CHECK(buf[0] == (2 << 1) && buf[1] == 'h' && buf[2] == 'i' && buf[3] == 0);
  dtor(buf);

  // Long string: is-long bit in word 0, size in word 1, owned NUL-terminated buffer.
  alignas(8) uint8_t big[24];
  std::string payload(64, 'x');
  init(big, payload.data(), 64);
  uint64_t w0 = 0, w1 = 0, w2 = 0;
  std::memcpy(&w0, big, 8); std::memcpy(&w1, big + 8, 8); std::memcpy(&w2, big + 16, 8);
  CHECK((w0 & 1) && w1 == 64 && (w0 >> 1) >= 64);
  CHECK(w2 != 0 && std::memcmp(reinterpret_cast<void*>(w2), payload.data(), 64) == 0);
  CHECK(reinterpret_cast<const char*>(w2)[64] == 0);
  dtor(big);  // frees the long buffer; ASan catches leaks/double frees

  // Default ctor, copy ctor, append, reserve, resize, insert, assign, push_back interoperate
  // and return non-null `this` (basic_string&) so PoolAllocator / mod_init never fault at 0x10.
  alignas(8) uint8_t a[24], b[24];
  using CtorFn = void* (*)(void*);
  using MutPtrLenFn = void* (*)(void*, const char*, uint64_t);
  using InsertCstrFn = void* (*)(void*, uint64_t, const char*);
  using InsertPtrLenFn = void* (*)(void*, uint64_t, const char*, uint64_t);
  using AssignCstrFn = void* (*)(void*, const char*);
  using PushBackFn = void (*)(void*, char);
  CHECK(reinterpret_cast<CtorFn>(*reg.resolve(S + "C1Ev", ""))(a) == a);
  CHECK(a[0] == 0);
  auto append = reinterpret_cast<MutPtrLenFn>(*reg.resolve(S + "6appendEPKcm", ""));
  CHECK(append(a, "abc", 3) == a);
  CHECK(a[0] == (3 << 1) && a[1] == 'a');
  // Exact symbol from the PoolAllocator faultAddr: 0x10 crash:
  auto insertCstr = reinterpret_cast<InsertCstrFn>(*reg.resolve(S + "6insertEmPKc", ""));
  CHECK(insertCstr(a, 0, "pre_") == a);
  CHECK(a[0] == (7 << 1) && std::memcmp(a + 1, "pre_abc", 7) == 0);
  auto insertPtrLen = reinterpret_cast<InsertPtrLenFn>(*reg.resolve(S + "6insertEmPKcm", ""));
  CHECK(insertPtrLen(a, 7, "_end", 4) == a);
  auto pushBack = reinterpret_cast<PushBackFn>(*reg.resolve(S + "9push_backEc", ""));
  pushBack(a, '!');
  CHECK(a[0] == (12 << 1) && std::memcmp(a + 1, "pre_abc_end!", 12) == 0);
  auto assignCstr = reinterpret_cast<AssignCstrFn>(*reg.resolve(S + "6assignEPKc", ""));
  CHECK(assignCstr(a, "abc") == a);
  reinterpret_cast<void* (*)(void*, const void*)>(*reg.resolve(S + "C1ERKS5_", ""))(b, a);
  CHECK(b[0] == (3 << 1) && b[1] == 'a');
  auto reserve = reinterpret_cast<void (*)(void*, uint64_t)>(*reg.resolve(S + "7reserveEm", ""));
  reserve(b, 100);
  uint64_t bw0 = 0; std::memcpy(&bw0, b, 8);
  CHECK((bw0 & 1) && (bw0 >> 1) >= 100);
  CHECK(append(b, "def", 3) == b);  // long-mode append keeps the contents and returns `this`
  CHECK(insertCstr(b, 0, "hdr:") == b);
  auto resize = reinterpret_cast<void (*)(void*, uint64_t, char)>(*reg.resolve(S + "6resizeEmc", ""));
  resize(b, 4, '_');     // truncate
  uint64_t bsz = 0; std::memcpy(&bsz, b + 8, 8);
  CHECK(bsz == 4);
  dtor(a); dtor(b);

  // ios_base::Init wrappers exist and run: the classic __GLOBAL__I_a dependency.
  alignas(8) uint8_t iosInit[8] = {};
  reinterpret_cast<VoidFn>(*reg.resolve("__ZNSt3__18ios_base4InitC1Ev", ""))(iosInit);
  reinterpret_cast<VoidFn>(*reg.resolve("__ZNSt3__18ios_base4InitD1Ev", ""))(iosInit);

  // npos is a data symbol bound to storage holding ~0.
  auto npos = reg.resolve(S + "4nposE", "");
  CHECK(npos && *reinterpret_cast<const uint64_t*>(*npos) == ~uint64_t(0));

  // dlsym pass-through discipline: only _Z-mangled names, only Apple C++ dylibs, honest misses.
  runtime::HostCxxResolver host;
  CHECK(!host.resolve("_puts", "").has_value());
  CHECK(!host.resolve("__ZNSt3__1definitely_missingEv", "").has_value());
  CHECK(!host.resolve("__ZNSt3__1definitely_missingEv", "/usr/lib/libSystem.B.dylib").has_value());
  CHECK(runtime::isAppleCxxDylib("/usr/lib/libc++.1.dylib"));
  CHECK(runtime::isAppleCxxDylib("/usr/lib/libc++abi.dylib"));
  CHECK(!runtime::isAppleCxxDylib("/usr/lib/libSystem.B.dylib"));
}

TEST(stub_dispatch_trampolines) {
  auto& arena = runtime::StubArena::instance();
  size_t before = arena.codeStubCount();
  uint64_t t1 = arena.makeCodeStub("_fake_missing_fn", "/usr/lib/UIKit.framework/UIKit");
  uint64_t t2 = arena.makeCodeStub("_fake_missing_fn", "/usr/lib/UIKit.framework/UIKit");
  CHECK(t1 != 0 && t1 == t2);  // deterministic across the loader's two passes
  CHECK(arena.codeStubCount() == before + 1);
  auto word = [&](int i) { uint32_t w; std::memcpy(&w, reinterpret_cast<void*>(t1 + i * 4), 4); return w; };
  CHECK(word(0) == 0x58000089);  // LDR x9,  [pc, #16]  -> StubRecord*
  CHECK(word(1) == 0x580000B0);  // LDR x16, [pc, #24]  -> dispatch entry
  CHECK(word(2) == 0xD61F0200);  // BR x16
  CHECK(a64::decode(word(0)).kind == a64::Kind::LdrLit);
  CHECK(a64::decode(word(2)).kind == a64::Kind::BR);
  uint64_t recAddr = 0, entry = 0;
  std::memcpy(&recAddr, reinterpret_cast<void*>(t1 + 16), 8);
  std::memcpy(&entry, reinterpret_cast<void*>(t1 + 24), 8);
  CHECK(entry == runtime::stubDispatchEntryAddress());
  const runtime::StubRecord* rec = arena.recordForStubAddress(t1);
  CHECK(rec && reinterpret_cast<uint64_t>(rec) == recAddr);
  CHECK(rec->symbol == "_fake_missing_fn" && rec->dylib.find("UIKit") != std::string::npos);
  CHECK(rec->ret == runtime::StubReturn::Zero);
  CHECK(!arena.recordForStubAddress(0x1234));

  runtime::takeGuestOutput();
  uint64_t r0 = runtime::radekiStubDispatchC(const_cast<runtime::StubRecord*>(rec), 0x1234);
  CHECK(r0 == 0 && rec->calls.load() == 1);
  CHECK(runtime::takeGuestOutput().find("_fake_missing_fn") != std::string::npos);

  runtime::setStubCallLogging(false);
  CHECK(runtime::radekiStubDispatchC(const_cast<runtime::StubRecord*>(rec), 0x5678) == 0);
  CHECK(runtime::takeGuestOutput().empty());
  CHECK(rec->calls.load() == 1);  // disabling diagnostics also avoids the per-call counter
  runtime::setStubCallLogging(true);

  // objc retain family passes x0 through; SEL helpers return a non-null empty selector.
  uint64_t t3 = arena.makeCodeStub("_objc_retain", "/usr/lib/libobjc.A.dylib");
  const runtime::StubRecord* r3 = arena.recordForStubAddress(t3);
  CHECK(r3 && r3->ret == runtime::StubReturn::Arg0);
  CHECK(runtime::radekiStubDispatchC(const_cast<runtime::StubRecord*>(r3), 0xDEAD) == 0xDEAD);
  uint64_t t4 = arena.makeCodeStub("_sel_registerName", "/usr/lib/libobjc.A.dylib");
  uint64_t sel = runtime::radekiStubDispatchC(const_cast<runtime::StubRecord*>(arena.recordForStubAddress(t4)), 0);
  CHECK(sel != 0 && *reinterpret_cast<const char*>(sel) == 0);

  // Data imports: dummy objc-class-shaped blocks with a diagnostic label.
  relinker::StubFactory factory = runtime::makeDispatchStubFactory();
  CHECK(runtime::stubLooksLikeData("_OBJC_CLASS_$_UIView"));
  CHECK(runtime::stubLooksLikeData("_kCFAllocatorDefault"));
  CHECK(runtime::stubLooksLikeData("_UIApplicationDidFinishLaunchingNotification"));
  CHECK(runtime::stubLooksLikeData("_NSConcreteStackBlock"));
  CHECK(!runtime::stubLooksLikeData("_UIGraphicsBeginImageContext"));
  uint64_t cls = factory("_OBJC_CLASS_$_UIView", "/System/Library/Frameworks/UIKit.framework/UIKit");
  CHECK(cls != 0 && arena.dataObjectLabel(cls) == "UIView");
  CHECK(factory("_OBJC_CLASS_$_UIView", "/System/Library/Frameworks/UIKit.framework/UIKit") == cls);
  const uint64_t* obj = reinterpret_cast<const uint64_t*>(cls);
  CHECK(obj[0] == 0 && obj[1] == 0 && obj[4] == 0);  // class fields all zero
  CHECK(arena.dataObjectLabel(0x1234).empty());

#if defined(__aarch64__)
  // On an ARM64 host the trampoline is live machine code: call it exactly like the guest.
  using StubFn = uint64_t (*)(uint64_t);
  CHECK(reinterpret_cast<StubFn>(t3)(0xABCD) == 0xABCD);  // _objc_retain pass-through
  CHECK(reinterpret_cast<StubFn>(t1)(77) == 0);           // default safe return
#endif
}

TEST(relinker_dispatch_stub_mode) {
  auto f = synth::build();
  auto img = macho::parseFile(f);
  relinker::LinkOptions o;
  o.loadBase = 0x240000000ull;
  o.stubFactory = runtime::makeDispatchStubFactory();
  auto li = relinker::link(img, o);
  CHECK(li.traps.empty());  // no BRK/SIGTRAP stubs remain
  CHECK(li.imports.size() == 2);
  for (const auto& b : li.imports) {
    CHECK(b.dispatchStub && !b.resolved);  // bound to a stub, still honestly unresolved
    CHECK(b.cls == compat::SymbolClass::NoopStub && b.method == "stub_dispatch");
    CHECK(!li.contains(b.target));          // host stub address, outside the guest image
    CHECK(b.dylib == "/usr/lib/libSystem.B.dylib");
  }
  CHECK(relinker::validate(li).ok());
  // Weak imports still bind to null, never to a stub.
  auto wimg = img;
  wimg.imports[1].weak = true;
  auto wl = relinker::link(wimg, o);
  CHECK(wl.traps.empty());
  CHECK(wl.imports[1].weak && wl.imports[1].target == 0 && !wl.imports[1].dispatchStub);
  // Default policy unchanged: honest named BRK traps.
  auto plain = relinker::link(img, {});
  CHECK(plain.traps.size() == 2);
  CHECK(plain.imports[0].method == "trap" && plain.imports[0].cls == compat::SymbolClass::Unsupported);
}

TEST(dyld_stub_mode_honest_reporting) {
  auto exeBytes = synth::build({0, 0, 0, true});
  auto exe = macho::parseFile(exeBytes);
  loader::Registry reg;
  CHECK(reg.add({"radeki", &exe, true}) == 0);
  auto compat = runtime::makeCompatRegistry();
  loader::Options o;
  o.fallback = &compat;
  o.stubFactory = runtime::makeDispatchStubFactory();
  auto lr = reg.linkAll(o);
  CHECK(lr.ok());
  CHECK(lr.unresolved.size() == 2);  // still reported: stubs never fake success
  bool warned = false;
  for (auto& w : lr.warnings)
    if (w.find("dispatch stubs") != std::string::npos) warned = true;
  CHECK(warned);
  const loader::Entry* e = reg.find("radeki");
  CHECK(e && e->linked && e->linkedImage.traps.empty());  // _puts bound, the rest stubbed
  for (auto& u : lr.unresolved) {
    CHECK(u.trap == 0 && u.stub != 0 && !u.weak);
    if (u.symbol == "_missing_fn") {
      const runtime::StubRecord* rec = runtime::StubArena::instance().recordForStubAddress(u.stub);
      CHECK(rec && rec->symbol == u.symbol);  // the stub knows its own name for logging
    }
  }
  // Determinism across a relink: dedup keeps the same stub addresses (pass 1 == pass 2).
  loader::Registry reg2;
  reg2.add({"radeki", &exe, true});
  auto lr2 = reg2.linkAll(o);
  CHECK(lr2.unresolved.size() == 2);
  for (auto& u1 : lr.unresolved)
    for (auto& u2 : lr2.unresolved)
      if (u1.symbol == u2.symbol) CHECK(u1.stub == u2.stub);
}

TEST(host_runtime_layer) {
  auto host = runtime::makeHostRuntime();
  CHECK(runtime::frameworkStubCount() > 0);
  // The MCPE scenario: UIKit / Foundation / EAGL / CoreFoundation imports resolve host-side.
  CHECK(host.registry.resolve("_UIApplicationMain", "/System/Library/Frameworks/UIKit.framework/UIKit"));
  CHECK(host.registry.resolve("_NSLog", "/System/Library/Frameworks/Foundation.framework/Foundation"));
  CHECK(host.registry.resolve("_OBJC_CLASS_$_EAGLContext", "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"));
  CHECK(host.registry.resolve("_OBJC_METACLASS_$_UIView", "/System/Library/Frameworks/UIKit.framework/UIKit"));
  CHECK(host.registry.resolve("_kCFAllocatorDefault", "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"));
  CHECK(host.registry.resolve("_NSConcreteStackBlock", "/usr/lib/libSystem.B.dylib"));
  auto eagl = host.registry.describe("_OBJC_CLASS_$_EAGLContext", "");
  CHECK(eagl && eagl->cls == compat::SymbolClass::NoopStub && eagl->framework == "OpenGLES");
  auto main = host.registry.describe("_UIApplicationMain", "");
  CHECK(main && main->cls == compat::SymbolClass::CompatibilityShim);
  auto kcf = host.registry.describe("_kCFAllocatorDefault", "");
  CHECK(kcf && kcf->method == "dummy_data");
  // The plain compat registry stays free of framework stubs (doctrine)...
  auto plain = runtime::makeCompatRegistry();
  CHECK(!plain.resolve("_UIApplicationMain", "").has_value());
  CHECK(!plain.resolve("_OBJC_CLASS_$_UIView", "").has_value());
  // ...but carries the libc++ forwarding table.
  CHECK(plain.resolve("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm", "").has_value());
  // Registry + factory together leave the synthetic image with zero traps.
  CHECK(host.stubFactory);
  auto f = synth::build();
  auto img = macho::parseFile(f);
  relinker::LinkOptions o;
  o.resolver = &host.registry;
  o.stubFactory = host.stubFactory;
  auto li = relinker::link(img, o);
  CHECK(li.traps.empty());
  size_t stubbed = 0;
  for (auto& b : li.imports) stubbed += b.dispatchStub;
  CHECK(stubbed == 1);  // _puts from the compat registry; _missing_fn on a logging stub
  // UIApplicationMain now runs an active native event loop that drives EGL/GLESv2 and the
  // CADisplayLink -tick: callback; requestExitUiLoop() asks it to return so the test can
  // observe the entry and exit messages without hanging.
  using UIMain = int (*)(int, char**, const void*, const void*);
  auto uiMain = reinterpret_cast<UIMain>(*host.registry.resolve("_UIApplicationMain", ""));
  runtime::takeGuestOutput();
  std::thread exiter([]{
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    runtime::requestExitUiLoop(0);
  });
  CHECK(uiMain(1, nullptr, nullptr, nullptr) == 0);
  exiter.join();
  std::string uiOutput = runtime::takeGuestOutput();
  CHECK(uiOutput.find("UIApplicationMain") != std::string::npos);
  CHECK(uiOutput.find("entering native event loop") != std::string::npos);
}

TEST(run_image_stub_mode) {
  auto f = synth::build();
  auto img = macho::parseFile(f);
  auto r = runtime::runImage(img, /*useStubs=*/true);
#if defined(__aarch64__)
  CHECK(r.ran && r.exitCode == 7 && r.output == "hello radeki\nhello radeki\n");
  CHECK(r.dispatchStubs == 1);  // _missing_fn stubbed, _puts bound, startup completed
#else
  CHECK(!r.ran && r.error.find("ARM64 host") != std::string::npos);  // honest on other hosts
#endif
}

TEST(sandbox_and_eagl_gles_bridge) {
  // 1. POSIX external storage sandboxing (/storage/emulated/0/RadekiOSNative/sandbox/)
  runtime::setSandboxBasePath("");
  CHECK(runtime::sandboxBasePath() == "/storage/emulated/0/RadekiOSNative/sandbox/");
  CHECK(runtime::ensureSandboxDirectories());
  auto dirs = runtime::standardSandboxDirectories();
  CHECK(dirs.size() == 5);
  CHECK(dirs[0] == "/storage/emulated/0/RadekiOSNative/sandbox/Documents/");
  CHECK(dirs[1] == "/storage/emulated/0/RadekiOSNative/sandbox/Documents/games/com.mojang/");
  CHECK(dirs[2] == "/storage/emulated/0/RadekiOSNative/sandbox/Library/Application Support/");
  CHECK(dirs[3] == "/storage/emulated/0/RadekiOSNative/sandbox/Library/Caches/");
  CHECK(dirs[4] == "/storage/emulated/0/RadekiOSNative/sandbox/tmp/");
  CHECK(runtime::translateGuestPath("/var/mobile/Applications/1234-ABCD/Documents/games/com.mojang") ==
        "/storage/emulated/0/RadekiOSNative/sandbox/Documents/games/com.mojang");
  CHECK(runtime::translateGuestPath("/private/var/mobile/Containers/Data/Application/UUID/Library/Caches/x.bin") ==
        "/storage/emulated/0/RadekiOSNative/sandbox/Library/Caches/x.bin");
  CHECK(runtime::translateGuestPath("/tmp/test.tmp") ==
        "/storage/emulated/0/RadekiOSNative/sandbox/tmp/test.tmp");
  CHECK(std::string(runtime::guestNSHomeDirectory()) == "/storage/emulated/0/RadekiOSNative/sandbox");
  const auto* docPath = static_cast<const char*>(runtime::guestNSSearchPathForDirectoriesInDomains(9, 1, 1));
  CHECK(docPath && std::string(docPath) == "/storage/emulated/0/RadekiOSNative/sandbox/Documents");

  // C/POSIX interceptors (mkdir, fopen, stat, access, open, chdir)
  auto host = runtime::makeHostRuntime();
  using MkdirFn = int (*)(const char*, unsigned int);
  using AccessFn = int (*)(const char*, int);
  using StatFn = int (*)(const char*, void*);
  using ChdirFn = int (*)(const char*);
  auto fnMkdir = reinterpret_cast<MkdirFn>(*host.registry.resolve("_mkdir", ""));
  auto fnAccess = reinterpret_cast<AccessFn>(*host.registry.resolve("_access", ""));
  auto fnStat = reinterpret_cast<StatFn>(*host.registry.resolve("_stat", ""));
  auto fnChdir = reinterpret_cast<ChdirFn>(*host.registry.resolve("_chdir", ""));
  fnMkdir("/var/mobile/Applications/MCPE/Documents/games", 0755);
  CHECK(fnAccess("/var/mobile/Applications/MCPE/Documents/games", 0) == 0);
  alignas(8) uint8_t darwinStat[144] = {};
  CHECK(fnStat("/var/mobile/Applications/MCPE/Documents/games", darwinStat) == 0);
  CHECK(fnChdir("/var/mobile/Applications/MCPE/Documents/games") == 0);

  // 2. EAGLContext / UIWindow / CADisplayLink & OpenGL ES 2.0 bridge
  using MsgSendFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  auto msgSend = reinterpret_cast<MsgSendFn>(*host.registry.resolve("_objc_msgSend", ""));
  uint64_t eaglCls = *host.registry.resolve("_OBJC_CLASS_$_EAGLContext", "");
  uint64_t ctx = msgSend(eaglCls, reinterpret_cast<uint64_t>("initWithAPI:"), 2, 0, 0, 0);
  CHECK(ctx != 0);
  CHECK(msgSend(eaglCls, reinterpret_cast<uint64_t>("setCurrentContext:"), ctx, 0, 0, 0) == 1);
  CHECK(msgSend(ctx, reinterpret_cast<uint64_t>("presentRenderbuffer:"), 0x8D41, 0, 0, 0) == 1);
  uint64_t winCls = *host.registry.resolve("_OBJC_CLASS_$_UIWindow", "");
  CHECK(msgSend(winCls, reinterpret_cast<uint64_t>("makeKeyAndVisible"), 0, 0, 0, 0) != 0);
  CHECK(runtime::uiWindowDefault()->keyAndVisible == 1);
  uint64_t dlCls = *host.registry.resolve("_OBJC_CLASS_$_CADisplayLink", "");
  uint64_t dl = msgSend(dlCls, reinterpret_cast<uint64_t>("displayLinkWithTarget:selector:"), 0x1111, 0x2222, 0, 0);
  CHECK(dl != 0);
  msgSend(dl, reinterpret_cast<uint64_t>("addToRunLoop:forMode:"), 1, 2, 0, 0);
  CHECK(runtime::caDisplayLinkDefault()->registeredInRunLoop == 1);

  CHECK(runtime::glesBridgeSymbolCount() >= 90);
  using GlGetStringFn = const char* (*)(uint32_t);
  auto glGetString = reinterpret_cast<GlGetStringFn>(*host.registry.resolve("_glGetString", ""));
  CHECK(glGetString && std::string(glGetString(0x1F02)).find("OpenGL ES") != std::string::npos);
}

int main() {
  bytes_bounds(); arm64_encodings(); fat_and_selection(); parse_dyld_info(); parse_chained(); malformed_inputs();
  analysis_report(); link_and_execute(); unresolved_import_traps_with_symbol(); link_refusals(); branch_veneer();
  validator_catches_escape(); objc_metadata(); objc_metadata_hostile_inputs(); objc_metadata_after_link();
  dyld_binds_across_images(); dyld_flat_namespace_late_provider(); dyld_keeps_unresolved_imports_honest();
  dyld_error_reporting(); dyld_run_loaded_images();
  compat_and_runtime(); batch2_status_and_cxx(); batch2_loader_classification(); batch2_ipa(); mutation_fuzz_no_crash(); json_writer();
  cxx_forward_string_wrappers(); stub_dispatch_trampolines(); relinker_dispatch_stub_mode();
  dyld_stub_mode_honest_reporting(); host_runtime_layer(); run_image_stub_mode();
  sandbox_and_eagl_gles_bridge();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
