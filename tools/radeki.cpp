// radeki CLI: analyze | convert | validate | load | run  (all support --json)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <filesystem>
#include "compat/matrix.h"
#include "ipa/bundle.h"
#include <string>

#include "binary_analysis/analysis.h"
#include "core/json.h"
#include "loader/dyld.h"
#include "objc/objc.h"
#include "relinker/relinker.h"
#include "runtime/runtime.h"

using namespace radeki;

static std::vector<uint8_t> slurp(const char* p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw FormatError(std::string("cannot open ") + p);
  return {std::istreambuf_iterator<char>(f), {}};
}

static void jsonImage(JsonWriter& j, const macho::Image& img, const analysis::Report& rep) {
  j.kv("arch", macho::archName(img.arch)).kvb("is64", img.is64).kv("uuid", img.uuid).kvu("segments", img.segments.size());
  if (img.entry) j.kvh("entry", *img.entry);
  j.kvb("chainedFixups", img.hasChainedFixups).kvu("fixups", img.fixups.size()).kvu("imports", img.imports.size());
  j.kvb("encrypted", img.cryptId != 0).kvb("codeSignature", img.hasCodeSignature).kvb("tls", img.hasTLS);
  j.kvb("objc", rep.objc).kvb("swift", rep.swift).kvb("metal", rep.metal).kvb("opengles", rep.gles).kvb("audio", rep.audio);
  j.kvu("instructions", rep.instructions).kvu("functions", rep.functions.size()).kvu("xrefs", rep.xrefs.size());
  j.kvu("pacSign", rep.pacSign).kvu("pacAuth", rep.pacAuth);
  j.key("frameworks").beginArray(); for (auto& f : rep.frameworks) j.str(f); j.endArray();
  j.key("importNames").beginArray(); for (auto& i : img.imports) j.str(i.name); j.endArray();
  j.key("blockers").beginArray(); for (auto& b : rep.blockers) j.str(b); j.endArray();
}

static void jsonMethods(JsonWriter& j, const char* key, const std::vector<objc::Method>& ms) {
  j.key(key).beginArray();
  for (auto& m : ms) j.beginObject().kv("sel", m.selector).kv("types", m.types).kvh("imp", m.imp).endObject();
  j.endArray();
}

// Objective-C metadata is read-only: it is reported, never registered or dispatched.
static void jsonObjc(JsonWriter& j, const macho::Image& img, bool dump) {
  auto md = objc::parse(img);
  j.kvb("objcPresent", md.present).kvb("objcComplete", md.complete);
  j.kvu("objcClasses", md.classes.size()).kvu("objcCategories", md.categories.size());
  j.kvu("objcProtocols", md.protocols.size()).kvu("objcMethods", md.methodCount());
  j.kvu("objcSelectors", md.selectorCount());
  if (!md.warnings.empty()) {
    j.key("objcWarnings").beginArray();
    for (auto& w : md.warnings) j.str(w);
    j.endArray();
  }
  if (!dump) return;
  j.key("classes").beginArray();
  for (auto& c : md.classes) {
    j.beginObject().kv("name", c.name).kv("super", c.superName).kvu("instanceSize", c.instanceSize);
    jsonMethods(j, "methods", c.methods);
    jsonMethods(j, "classMethods", c.classMethods);
    j.key("ivars").beginArray();
    for (auto& v : c.ivars) j.beginObject().kv("name", v.name).kv("type", v.type).kvu("offset", v.offset).kvu("size", v.size).endObject();
    j.endArray();
    j.key("properties").beginArray();
    for (auto& p : c.properties) j.beginObject().kv("name", p.name).kv("attrs", p.attrs).endObject();
    j.endArray();
    j.key("protocols").beginArray(); for (auto& x : c.protocols) j.str(x); j.endArray();
    j.endObject();
  }
  j.endArray();
  j.key("categories").beginArray();
  for (auto& c : md.categories) {
    j.beginObject().kv("name", c.name).kv("class", c.className);
    jsonMethods(j, "methods", c.methods);
    jsonMethods(j, "classMethods", c.classMethods);
    j.endObject();
  }
  j.endArray();
  j.key("protocols").beginArray();
  for (auto& p : md.protocols) {
    j.beginObject().kv("name", p.name);
    jsonMethods(j, "methods", p.methods);
    j.key("protocols").beginArray(); for (auto& x : p.protocols) j.str(x); j.endArray();
    j.endObject();
  }
  j.endArray();
  j.key("selectors").beginArray();
  for (auto& x : md.selectors) j.str(x);
  j.endArray();
}

static void usage() {
  fprintf(stderr,
          "usage: radeki analyze|convert|validate|symbols <macho> [-o dir] [--load-base 0x..] [--json] [--objc] [--stubs]\n"
          "       radeki ipa <App.app> [-o dir] [--json]\n"
          "       radeki load|run <exe> [--dylib <path>]... [--load-base 0x..] [--json] [--stubs|--traps]\n"
          "  ipa       inspect an extracted .app and convert its real icon to a standard PNG\n"
          "  symbols   list every bound import with classification and resolution method\n"
          "  analyze   static report; --objc dumps the Objective-C metadata it found\n"
          "  convert   link and write the relinked image; validate checks every PC-relative reference\n"
          "  load      register <exe> plus every --dylib, link them together and report unresolved imports\n"
          "  run       the same, then execute them in one address space (needs a matching ARM process)\n"
          "  --stubs   link with the host runtime layer: framework shims/dummies + libc++ forwarding, and\n"
          "            bind what remains to logging no-op dispatch stubs instead of BRK traps (opt-in for\n"
          "            load/symbols; run uses it by default, --traps restores honest trap stubs).\n"
          "            Unresolved imports stay listed as unresolved either way.\n"
          "exit codes: 0 ok, 1 error, 2 usage, 3 linked but some imports had no provider (trap stubs kept)\n");
}

int main(int argc, char** argv) {
  if (argc < 3) { usage(); return 2; }
  std::string cmd = argv[1];
  bool json = false, objcDump = false, baseSet = false, stubsFlag = false, trapsFlag = false;
  const char* outDir = nullptr;
  uint64_t base = relinker::LinkOptions{}.loadBase;
  std::vector<std::string> dylibPaths;
  for (int i = 3; i < argc; ++i) {
    if (!strcmp(argv[i], "--json")) json = true;
    else if (!strcmp(argv[i], "--objc")) objcDump = true;
    else if (!strcmp(argv[i], "--stubs")) stubsFlag = true;
    else if (!strcmp(argv[i], "--traps")) trapsFlag = true;
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) outDir = argv[++i];
    else if (!strcmp(argv[i], "--load-base") && i + 1 < argc) { base = strtoull(argv[++i], nullptr, 0); baseSet = true; }
    else if (!strcmp(argv[i], "--dylib") && i + 1 < argc) dylibPaths.push_back(argv[++i]);
    else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
  }
  if (stubsFlag && trapsFlag) { fprintf(stderr, "--stubs and --traps are mutually exclusive\n"); return 2; }
  try {
    if (cmd == "ipa") {
      ipa::BundleInfo info;std::string error;
      std::filesystem::path dest = outDir ? outDir : ".";
      if(!ipa::inspectBundle(argv[2],(dest/"icon.png").string(),info,error)) throw FormatError(error);
      JsonWriter j;j.beginObject().kv("bundleId",info.bundleIdentifier).kv("name",info.name)
        .kv("executable",info.executable).kv("executablePath",info.executablePath)
        .kvu("executableSize",info.executableSize).kv("version",info.version)
        .kv("shortVersion",info.shortVersion).kv("minimumOSVersion",info.minimumOSVersion)
        .kv("platform",info.platform).kv("iconPath",info.iconPath);
      j.key("architectures").beginArray();for(const auto& a:info.architectures)j.str(a);j.endArray();
      j.key("warnings").beginArray();for(const auto& w:info.warnings)j.str(w);j.endArray();
      j.endObject();puts(j.str().c_str());return 0;
    }
    auto file = slurp(argv[2]);
    auto slices = macho::listSlices(file);
    const bool nativeLoadCommand = cmd == "load" || cmd == "run";
    auto choice = nativeLoadCommand ? macho::chooseSliceForHost(slices) : macho::chooseSlice(slices);
    if (!choice.index) throw FormatError(choice.reason);
    auto img = macho::parseSlice(file, slices[*choice.index]);
    auto rep = analysis::analyze(img);
    JsonWriter j; j.beginObject().kv("command", cmd).kv("sliceChoice", choice.reason);
    if (cmd == "analyze") {
      jsonImage(j, img, rep);
      jsonObjc(j, img, objcDump);
      compat::Detected d;d.objc=rep.objc;d.gles=rep.gles;d.metal=rep.metal;
      d.audio=rep.audio;d.swift=rep.swift;
      for(const auto& f:rep.frameworks){
        if(f.find("UIKit")!=std::string::npos)d.uikit=true;
        if(f.find("CoreFoundation")!=std::string::npos)d.corefoundation=true;
        if(f.find("Vulkan")!=std::string::npos)d.vulkan=true;
      }d.arm64e=img.arch==macho::Arch::ARM64e;
      d.armv7=img.arch==macho::Arch::ARMv7;
#if defined(__arm__) && !defined(__aarch64__)
      d.hostArm32=true;
#endif
      d.encrypted=img.cryptId!=0;
      if(d.encrypted)d.blockedReason="encrypted executable";
      // Analyze does not imply linking; state remains ANALYZED until an actual link succeeds.
      j.kv("state",compat::runtimeStateName(compat::stateFor(d))).kv("summary",compat::summaryFor(d,img.imports.size()));
      j.key("capabilities").beginArray();
      for(const auto& row:compat::neededCapabilities(d))
        j.beginObject().kv("id",row.id).kv("title",row.title).kv("subsystem",row.subsystem)
          .kv("status",compat::statusName(row.status)).kv("detail",row.detail).endObject();
      j.endArray();
    } else if (cmd == "symbols") {
      auto registry=runtime::makeCompatRegistry();
      runtime::HostRuntime host;
      if (stubsFlag) host = runtime::makeHostRuntime();
      relinker::LinkOptions opt;opt.loadBase=base;
      opt.resolver=stubsFlag ? static_cast<const relinker::ImportResolver*>(&host.registry) : &registry;
      if (stubsFlag) opt.stubFactory = host.stubFactory;
      auto li=relinker::link(img,opt);
      j.key("imports").beginArray();
      for(const auto& imp:li.imports)
        j.beginObject().kv("symbol",imp.symbol).kv("dylib",imp.dylib)
          .kv("classification",compat::symbolClassName(imp.cls)).kv("framework",imp.framework)
          .kv("method",imp.method).kvh("slot",imp.slot).kvh("target",imp.target).endObject();
      j.endArray();
    } else if (cmd == "load" || cmd == "run") {
      // The dylibs and their bytes have to outlive the registry, hence the deques: Entry holds
      // pointers into them, and a vector would move them on growth.
      std::deque<std::vector<uint8_t>> bufs;
      std::deque<macho::Image> imgs;
      loader::Registry reg;
      if (reg.add({argv[2], &img, true}) < 0) throw FormatError("could not register the executable");
      for (const auto& p : dylibPaths) {
        bufs.push_back(slurp(p.c_str()));
        auto sl = macho::listSlices(bufs.back());
        auto ch = nativeLoadCommand ? macho::chooseSliceForHost(sl) : macho::chooseSlice(sl);
        if (!ch.index) throw FormatError(p + ": " + ch.reason);
        imgs.push_back(macho::parseSlice(bufs.back(), sl[*ch.index]));
        if (reg.add({p, &imgs.back(), false}) < 0) throw FormatError(p + ": could not be registered (duplicate path?)");
      }
      auto compat = runtime::makeCompatRegistry();
      runtime::HostRuntime host;
      // run defaults to the host runtime layer (stubs keep the startup loop alive); load and
      // report keep honest trap stubs unless --stubs asks for the dispatch-stub mode.
      const bool useStubs = cmd == "run" ? !trapsFlag : stubsFlag;
      if (useStubs) host = runtime::makeHostRuntime();
      loader::Options o;
      if (baseSet) o.firstBase = base;
      o.fallback = useStubs ? static_cast<const relinker::ImportResolver*>(&host.registry) : &compat;
      if (useStubs) o.stubFactory = host.stubFactory;
      j.kvu("images", reg.size());
      j.kvb("dispatchStubMode", useStubs);
      if (cmd == "load") {
        auto lr = reg.linkAll(o);
        j.kvb("ok", lr.ok());
        j.key("order").beginArray();
        for (auto* e : lr.order)
          j.beginObject().kv("path", e->path).kvh("loadBase", e->loadBase).kvb("linked", e->linked)
              .kvu("imageSize", e->imageSize).kvu("totalSize", e->totalSize).endObject();
        j.endArray();
        j.key("unresolved").beginArray();
        for (auto& u : lr.unresolved)
          j.beginObject().kv("path", u.path).kv("symbol", u.symbol).kv("dylib", u.dylib)
              .kvb("weak", u.weak).kvh("trap", u.trap).kvh("stub", u.stub).endObject();
        j.endArray();
        j.key("errors").beginArray(); for (auto& e : lr.errors) j.str(e); j.endArray();
        j.key("warnings").beginArray(); for (auto& w : lr.warnings) j.str(w); j.endArray();
        j.endObject();
        puts(j.str().c_str());
        // 3 means "loaded, but something is still unbound": honest, and distinct from a failure.
        return lr.ok() ? (lr.unresolved.empty() ? 0 : 3) : 1;
      }
      auto mr = runtime::runLoadedImage(reg, o);
      j.kvb("ok", mr.ok()).kvb("ran", mr.ran).kvb("crashed", mr.crashed);
      j.key("exitCode").num(mr.exitCode);
      j.kv("error", mr.error).kv("output", mr.output);
      j.kvu("dispatchStubs", mr.dispatchStubs);
      j.key("stubbedImports").beginArray();
      for (auto& s : mr.stubbed) j.str(s);
      j.endArray();
      j.key("images").beginArray();
      for (auto& ir : mr.images)
        j.beginObject().kv("path", ir.path).kvb("ran", ir.ran).kvb("crashed", ir.crashed)
            .kvb("skipped", ir.skipped).key("exitCode").num(ir.exitCode).kv("signal", ir.signalName)
            .kv("trapSymbol", ir.trapSymbol).kv("error", ir.error).endObject();
      j.endArray();
      j.endObject();
      puts(j.str().c_str());
      return mr.ok() ? 0 : 1;
    } else if (cmd == "convert" || cmd == "validate") {
      relinker::CompatRegistry none;  // no compatibility libraries exist yet: every import is reported unresolved
      relinker::LinkOptions o; o.loadBase = base; o.resolver = &none;
      auto li = relinker::link(img, o);
      auto v = relinker::validate(li);
      j.kvh("loadBase", li.loadBase).kvu("imageSize", li.imageSize).kvu("rebases", li.rebases).kvu("binds", li.binds);
      if (li.entry) j.kvh("entry", *li.entry);
      j.kvb("validated", v.ok()).kvu("referencesChecked", v.checked);
      j.key("violations").beginArray(); for (auto& x : v.violations) j.beginObject().kvh("addr", x.addr).kv("what", x.what).endObject(); j.endArray();
      j.key("unresolvedImports").beginArray(); for (auto& t : li.traps) j.beginObject().kv("symbol", t.symbol).kvh("trap", t.addr).endObject(); j.endArray();
      if (cmd == "convert") {
        if (!outDir) throw FormatError("convert needs -o <dir>");
        std::string d = outDir;
        std::ofstream(d + "/image.bin", std::ios::binary).write((const char*)li.memory.data(), li.memory.size());
      }
      if (!v.ok()) { j.endObject(); puts(j.str().c_str()); return 1; }
    } else throw FormatError("unknown command");
    j.endObject();
    if (json) puts(j.str().c_str());
    else puts(j.str().c_str());  // human-readable formatting is a later task; JSON is authoritative
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
