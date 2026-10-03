#include "objc/objc.h"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "core/bytes.h"

namespace radeki::objc {
namespace {

using macho::Image;
using macho::Section;

constexpr uint32_t kRoMeta = 1u << 29;                    // class_ro_t::flags
constexpr uint64_t kFastDataMask = 0x00007ffffffffff8ull;  // class_data_bits_t::FAST_DATA_MASK (arm64)
constexpr uint64_t kMaxListEntries = 1u << 20;
constexpr size_t kMaxWarnings = 64;
constexpr size_t kMaxString = 4096;

// class object (objc_class): isa, superclass, cache_t (16 bytes), class_data_bits_t.
constexpr uint64_t kClassSuper = 8, kClassBits = 32;
// class_ro_t (LP64).
constexpr uint64_t kRoFlags = 0, kRoInstanceSize = 8, kRoName = 24, kRoMethods = 32, kRoProtocols = 40,
                   kRoIvars = 48, kRoProperties = 64, kRoSize = 72;
// protocol_t: isa, mangledName, protocols, instanceMethods, ...
constexpr uint64_t kProtoName = 8, kProtoProtocols = 16, kProtoMethods = 24;
// category_t (classic layout).
constexpr uint64_t kCatName = 0, kCatCls = 8, kCatInstMethods = 16, kCatClassMethods = 24, kCatProtocols = 32;

std::string hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

class Parser {
 public:
  explicit Parser(const Image& img) : img_(img) {}

  Metadata run() {
    for (const auto& seg : img_.segments)
      for (const auto& sec : seg.sections)
        if (sec.name.rfind("__objc_", 0) == 0) {
          m_.sections.push_back(sec.name);
          secs_.push_back(&sec);
        }
    if (m_.sections.empty()) return m_;
    m_.present = true;
    if (!img_.is64) {
      warn("32-bit Objective-C metadata is not parsed (only arm64 metadata layouts are known)");
      return m_;
    }
    if (const Section* s = find("__objc_methname")) readStrings(*s, m_.selectors);

    walkPtrList("__objc_classlist", [&](uint64_t a) {
      if (seen(a)) return;
      visited_.push_back(a);
      Class c = parseClass(a);
      // __objc_classlist can hold metaclasses in odd images; keep the flat list honest.
      m_.classes.push_back(std::move(c));
    });
    walkPtrList("__objc_catlist", [&](uint64_t a) { m_.categories.push_back(parseCategory(a)); });
    walkPtrList("__objc_protolist", [&](uint64_t a) { m_.protocols.push_back(parseProtocol(a)); });
    return m_;
  }

 private:
  const Image& img_;
  Metadata m_;
  std::vector<const Section*> secs_;
  std::vector<uint64_t> visited_;

  void warn(const std::string& w) {
    m_.complete = false;
    if (m_.warnings.size() < kMaxWarnings) m_.warnings.push_back(w);
  }
  bool seen(uint64_t a) const { return std::find(visited_.begin(), visited_.end(), a) != visited_.end(); }
  const Section* find(const std::string& n) const {
    for (const auto* s : secs_) if (s->name == n) return s;
    return nullptr;
  }

  // ---- bounds-checked readers ------------------------------------------------
  bool readable(uint64_t vm, uint64_t len) const { return img_.vmToFile(vm, len).has_value(); }

  bool u32(uint64_t vm, uint32_t& out) const {
    auto fo = img_.vmToFile(vm, 4);
    if (!fo) return false;
    out = Reader(img_.data).read<uint32_t>(*fo, "objc u32");
    return true;
  }
  bool u64(uint64_t vm, uint64_t& out) const {
    auto fo = img_.vmToFile(vm, 8);
    if (!fo) return false;
    out = Reader(img_.data).read<uint64_t>(*fo, "objc u64");
    return true;
  }
  bool strAt(uint64_t vm, std::string& out) const {
    auto fo = img_.vmToFile(vm, 1);
    if (!fo) return false;
    try {
      out = Reader(img_.data).cstr(*fo, img_.data.size(), "objc string");
    } catch (const FormatError&) {
      return false;
    }
    if (out.size() > kMaxString) out.resize(kMaxString);
    return true;
  }

  void readStrings(const Section& s, std::vector<std::string>& out) {
    auto fo = img_.vmToFile(s.addr, s.size);
    if (!fo) { warn("section " + s.name + " is not file-backed; strings not read"); return; }
    std::string cur;
    for (uint64_t i = 0; i < s.size; ++i) {
      uint8_t c = Reader(img_.data).read<uint8_t>(*fo + i, "objc string byte");
      if (!c) {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
        if (out.size() >= (1u << 16)) break;
        continue;
      }
      if (cur.size() < kMaxString) cur.push_back(static_cast<char>(c));
    }
    if (!cur.empty()) out.push_back(cur);
  }

  void walkPtrList(const char* name, const std::function<void(uint64_t)>& fn) {
    const Section* s = find(name);
    if (!s) return;
    if (s->size < 8) return;
    if (!readable(s->addr, s->size)) {
      warn(std::string("section ") + name + " is not file-backed; skipped");
      return;
    }
    uint64_t n = s->size / 8;
    if (n > kMaxListEntries) { warn(std::string("section ") + name + " is implausibly large"); return; }
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t a = 0;
      if (!u64(s->addr + i * 8, a)) continue;
      if (!a) continue;
      fn(a);
      if (m_.classes.size() + m_.categories.size() + m_.protocols.size() > (1u << 16)) { warn(std::string("section ") + name + " truncated: too many entries"); return; }
    }
  }

  // ---- metadata structures ---------------------------------------------------
  // class object -> class_ro_t. Returns false (and lets the caller warn) for tagged/PAC
  // pointers or for a ro that is not fully mapped.
  bool classData(uint64_t addr, uint64_t& ro) const {
    uint64_t bits = 0;
    if (!u64(addr + kClassBits, bits)) return false;
    if (bits & ~kFastDataMask) return false;
    ro = bits;
    return ro != 0 && readable(ro, kRoSize);
  }

  bool className(uint64_t addr, std::string& out) const {
    uint64_t ro = 0, namePtr = 0;
    if (!classData(addr, ro)) return false;
    if (!u64(ro + kRoName, namePtr)) return false;
    return strAt(namePtr, out);
  }

  struct ListHeader { uint32_t count = 0, entsize = 0; };
  bool listHeader(uint64_t addr, ListHeader& h) const {
    uint32_t e = 0, c = 0;
    if (!u32(addr, e) || !u32(addr + 4, c)) return false;
    h.entsize = (e & 0xFFFFu) & ~3u;  // low 2 bits are flags, high 16 bits are flags
    h.count = c;
    return true;
  }

  bool selectorAt(uint64_t nameAddr, std::string& out) const {
    if (strAt(nameAddr, out)) return true;
    // Old-style images keep a SEL slot that points at the name; try exactly one indirection.
    uint64_t slot = 0;
    if (u64(nameAddr, slot) && strAt(slot, out)) return true;
    return false;
  }

  std::vector<Method> methods(uint64_t addr) {
    std::vector<Method> out;
    if (!addr) return out;
    ListHeader h;
    if (!listHeader(addr, h)) { warn("method list at " + hex(addr) + " is not readable"); return out; }
    if (h.entsize != 24 && h.entsize != 12) {
      warn("method list at " + hex(addr) + " has unsupported entry size " + std::to_string(h.entsize) + " (expected 24 or 12)");
      return out;
    }
    if (h.count > kMaxListEntries) { warn("method list at " + hex(addr) + " has implausible count " + std::to_string(h.count)); return out; }
    uint64_t bytes = 8ull + uint64_t(h.count) * h.entsize;
    if (!readable(addr, bytes)) { warn("method list at " + hex(addr) + " extends outside the mapped image"); return out; }
    const bool rel = h.entsize == 12;
    for (uint32_t i = 0; i < h.count; ++i) {
      uint64_t base = addr + 8 + uint64_t(i) * h.entsize;
      Method m;
      uint64_t nameAddr = 0, typesAddr = 0;
      if (rel) {
        uint32_t rn = 0, rt = 0, ri = 0;
        if (!u32(base, rn) || !u32(base + 4, rt) || !u32(base + 8, ri)) { warn("unreadable relative method entry at " + hex(base)); continue; }
        nameAddr = base + uint64_t(int64_t(int32_t(rn)));
        typesAddr = base + 4 + uint64_t(int64_t(int32_t(rt)));
        m.imp = base + 8 + uint64_t(int64_t(int32_t(ri)));
      } else {
        uint64_t an = 0, at = 0, ai = 0;
        if (!u64(base, an) || !u64(base + 8, at) || !u64(base + 16, ai)) { warn("unreadable method entry at " + hex(base)); continue; }
        nameAddr = an;
        typesAddr = at;
        m.imp = ai;
      }
      if (!selectorAt(nameAddr, m.selector)) {
        warn("unreadable selector for method entry at " + hex(base));
        m.selector.clear();
      }
      if (!strAt(typesAddr, m.types)) m.types.clear();
      out.push_back(std::move(m));
    }
    return out;
  }

  std::vector<Ivar> ivars(uint64_t addr) {
    std::vector<Ivar> out;
    if (!addr) return out;
    ListHeader h;
    if (!listHeader(addr, h)) { warn("ivar list at " + hex(addr) + " is not readable"); return out; }
    if (h.entsize != 32 && h.entsize != 20) {
      warn("ivar list at " + hex(addr) + " has unsupported entry size " + std::to_string(h.entsize) + " (expected 32 or 20)");
      return out;
    }
    if (h.count > kMaxListEntries) { warn("ivar list at " + hex(addr) + " has implausible count " + std::to_string(h.count)); return out; }
    if (!readable(addr, 8ull + uint64_t(h.count) * h.entsize)) { warn("ivar list at " + hex(addr) + " extends outside the mapped image"); return out; }
    const bool rel = h.entsize == 20;
    for (uint32_t i = 0; i < h.count; ++i) {
      uint64_t base = addr + 8 + uint64_t(i) * h.entsize;
      Ivar v;
      uint64_t offSlot = 0, nameAddr = 0, typeAddr = 0;
      if (rel) {
        uint32_t ro = 0, rn = 0, rt = 0;
        if (!u32(base, ro) || !u32(base + 4, rn) || !u32(base + 8, rt)) { warn("unreadable relative ivar entry at " + hex(base)); continue; }
        offSlot = base + uint64_t(int64_t(int32_t(ro)));
        nameAddr = base + 4 + uint64_t(int64_t(int32_t(rn)));
        typeAddr = base + 8 + uint64_t(int64_t(int32_t(rt)));
        u32(base + 16, v.size);
      } else {
        uint64_t ao = 0, an = 0, at = 0;
        if (!u64(base, ao) || !u64(base + 8, an) || !u64(base + 16, at)) { warn("unreadable ivar entry at " + hex(base)); continue; }
        offSlot = ao;
        nameAddr = an;
        typeAddr = at;
        u32(base + 28, v.size);
      }
      uint32_t off = 0;
      if (u32(offSlot, off)) v.offset = off;
      if (!strAt(nameAddr, v.name)) { warn("unreadable ivar name at " + hex(base)); v.name.clear(); }
      if (!strAt(typeAddr, v.type)) v.type.clear();
      out.push_back(std::move(v));
    }
    return out;
  }

  std::vector<Property> properties(uint64_t addr) {
    std::vector<Property> out;
    if (!addr) return out;
    ListHeader h;
    if (!listHeader(addr, h)) { warn("property list at " + hex(addr) + " is not readable"); return out; }
    if (h.entsize != 16 && h.entsize != 8) {
      warn("property list at " + hex(addr) + " has unsupported entry size " + std::to_string(h.entsize) + " (expected 16 or 8)");
      return out;
    }
    if (h.count > kMaxListEntries) { warn("property list at " + hex(addr) + " has implausible count " + std::to_string(h.count)); return out; }
    if (!readable(addr, 8ull + uint64_t(h.count) * h.entsize)) { warn("property list at " + hex(addr) + " extends outside the mapped image"); return out; }
    const bool rel = h.entsize == 8;
    for (uint32_t i = 0; i < h.count; ++i) {
      uint64_t base = addr + 8 + uint64_t(i) * h.entsize;
      Property p;
      uint64_t nameAddr = 0, attrAddr = 0;
      if (rel) {
        uint32_t rn = 0, ra = 0;
        if (!u32(base, rn) || !u32(base + 4, ra)) { warn("unreadable relative property entry at " + hex(base)); continue; }
        nameAddr = base + uint64_t(int64_t(int32_t(rn)));
        attrAddr = base + 4 + uint64_t(int64_t(int32_t(ra)));
      } else {
        uint64_t an = 0, aa = 0;
        if (!u64(base, an) || !u64(base + 8, aa)) { warn("unreadable property entry at " + hex(base)); continue; }
        nameAddr = an;
        attrAddr = aa;
      }
      if (!strAt(nameAddr, p.name) || !strAt(attrAddr, p.attrs)) { warn("unreadable property at " + hex(base)); continue; }
      out.push_back(std::move(p));
    }
    return out;
  }

  bool protocolName(uint64_t protoAddr, std::string& out) const {
    uint64_t namePtr = 0;
    if (!u64(protoAddr + kProtoName, namePtr)) return false;
    std::string s;
    if (!strAt(namePtr, s) || s.empty()) return false;
    out = std::move(s);
    return true;
  }

  // protocol_list_t exists in an absolute (uintptr_t count + pointers) and a relative
  // (uint32_t count + int32 offsets) encoding. Both are validated against the mapped image
  // instead of being guessed at; an ambiguity is reported, not hidden.
  std::vector<std::string> protocolNames(uint64_t addr) {
    std::vector<std::string> out;
    if (!addr) return out;
    if (!readable(addr, 8)) { warn("protocol list at " + hex(addr) + " is not readable"); return out; }

    auto absolute = [&](std::vector<std::string>& r) {
      uint64_t count = 0;
      if (!u64(addr, count) || count == 0 || count > kMaxListEntries) return false;
      if (!readable(addr + 8, count * 8)) return false;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t p = 0;
        std::string n;
        if (!u64(addr + 8 + i * 8, p) || !p || !protocolName(p, n)) return false;
        r.push_back(std::move(n));
      }
      return true;
    };
    auto relative = [&](std::vector<std::string>& r) {
      uint32_t count = 0;
      if (!u32(addr, count) || count == 0 || count > kMaxListEntries) return false;
      if (!readable(addr + 4, uint64_t(count) * 4)) return false;
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t e = addr + 4 + uint64_t(i) * 4;
        uint32_t off = 0;
        std::string n;
        if (!u32(e, off)) return false;
        uint64_t p = e + uint64_t(int64_t(int32_t(off)));
        if (!protocolName(p, n)) return false;
        r.push_back(std::move(n));
      }
      return true;
    };

    std::vector<std::string> a, b;
    bool okA = absolute(a), okB = relative(b);
    if (okA && okB && a != b) {
      warn("protocol list at " + hex(addr) + " validates as both an absolute and a relative list (" +
           std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " entries); using the absolute encoding");
      return a;
    }
    if (okA) return a;
    if (okB) return b;
    warn("protocol list at " + hex(addr) + " could not be read in either encoding");
    return out;
  }

  void fillFromRo(uint64_t ro, Class& c) {
    uint32_t flags = 0, instanceSize = 0;
    if (u32(ro + kRoFlags, flags) && (flags & kRoMeta))
      warn("class_ro_t at " + hex(ro) + " is a metaclass (RO_META set) but sits in __objc_classlist");
    if (u32(ro + kRoInstanceSize, instanceSize)) c.instanceSize = instanceSize;
    uint64_t namePtr = 0, ml = 0, pl = 0, il = 0, prl = 0;
    u64(ro + kRoName, namePtr);
    u64(ro + kRoMethods, ml);
    u64(ro + kRoProtocols, pl);
    u64(ro + kRoIvars, il);
    u64(ro + kRoProperties, prl);
    if (!strAt(namePtr, c.name)) {
      warn("class_ro_t at " + hex(ro) + " has an unreadable name pointer");
      c.name = "<unreadable>";
    }
    c.methods = methods(ml);
    c.protocols = protocolNames(pl);
    c.ivars = ivars(il);
    c.properties = properties(prl);
  }

  Class parseClass(uint64_t addr) {
    Class c;
    c.addr = addr;
    uint64_t isa = 0, superAddr = 0, bits = 0;
    if (!u64(addr, isa) || !u64(addr + kClassSuper, superAddr) || !u64(addr + kClassBits, bits)) {
      warn("class object at " + hex(addr) + " is not fully mapped");
      c.name = "<unreadable>";
      return c;
    }
    uint64_t ro = 0;
    if (!classData(addr, ro)) {
      warn("class object at " + hex(addr) + " has an unreadable class_ro_t pointer " + hex(bits) +
           (bits & ~kFastDataMask ? " (tag or PAC bits stripped by the mask)" : ""));
      c.name = "<unreadable>";
      return c;
    }
    fillFromRo(ro, c);
    if (superAddr) {
      std::string sn;
      if (className(superAddr, sn)) c.superName = sn;
      else warn("superclass object at " + hex(superAddr) + " of class " + c.name + " could not be read");
    }
    if (isa) {
      // Class methods are the metaclass's instance methods. One level only: no recursion,
      // so a malformed isa cannot walk us in circles.
      uint64_t mro = 0, ml = 0;
      if (classData(isa, mro) && u64(mro + kRoMethods, ml)) {
        c.classMethods = methods(ml);
      } else {
        warn("metaclass at " + hex(isa) + " of class " + c.name + " could not be read");
      }
    }
    return c;
  }

  Category parseCategory(uint64_t addr) {
    Category c;
    c.addr = addr;
    uint64_t namePtr = 0, clsPtr = 0, im = 0, cm = 0, pr = 0;
    if (!u64(addr + kCatName, namePtr) || !u64(addr + kCatCls, clsPtr) || !u64(addr + kCatInstMethods, im) ||
        !u64(addr + kCatClassMethods, cm) || !u64(addr + kCatProtocols, pr)) {
      warn("category at " + hex(addr) + " is not fully mapped");
      return c;
    }
    if (!strAt(namePtr, c.name)) {
      warn("category at " + hex(addr) + " has an unreadable name pointer");
      c.name = "<unreadable>";
    }
    if (clsPtr) {
      std::string cn;
      if (className(clsPtr, cn)) c.className = cn;
      else warn("category " + c.name + " targets an unreadable class object at " + hex(clsPtr));
    }
    c.methods = methods(im);
    c.classMethods = methods(cm);
    c.protocols = protocolNames(pr);
    return c;
  }

  Protocol parseProtocol(uint64_t addr) {
    Protocol p;
    p.addr = addr;
    uint64_t namePtr = 0, sub = 0, im = 0;
    if (!u64(addr + kProtoName, namePtr) || !u64(addr + kProtoProtocols, sub) || !u64(addr + kProtoMethods, im)) {
      warn("protocol at " + hex(addr) + " is not fully mapped");
      return p;
    }
    if (!strAt(namePtr, p.name)) {
      warn("protocol at " + hex(addr) + " has an unreadable name pointer");
      p.name = "<unreadable>";
    }
    p.methods = methods(im);
    p.protocols = protocolNames(sub);
    return p;
  }
};

}  // namespace

size_t Metadata::methodCount() const {
  size_t n = 0;
  for (const auto& c : classes) n += c.methods.size() + c.classMethods.size();
  for (const auto& c : categories) n += c.methods.size() + c.classMethods.size();
  for (const auto& p : protocols) n += p.methods.size();
  return n;
}

Metadata parse(const Image& img) { return Parser(img).run(); }

}  // namespace radeki::objc
