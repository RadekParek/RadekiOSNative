// Explicit libc++ forwarding wrappers. See cxx_forward.h for the doctrine.
//
// Every basic_string wrapper below implements the documented libc++ 64-bit storage layout
// explicitly (24 bytes: short mode holds size<<1 in byte 0 and up to 22 chars inline; long
// mode holds (capacity<<1)|1 in word 0, size in word 1 and the data pointer in word 2).
// They never reinterpret the host's std::string, because the host may be libstdc++, whose
// layout differs; on an Android NDK host the layouts agree anyway.
//
// CRITICAL ABI REQUIREMENT:
// In the Itanium/ARM64 C++ ABI used by Apple's libc++.1.dylib, mutating basic_string methods
// (insert, append, assign, replace, erase, operator=) return `basic_string&` in x0 (`this`),
// and constructors/destructors (C1/C2/D1/D2) also return `this` in x0 under ARM64 ABI.
// Returning void (or stubbing to 0) leaves x0 NULL or clobbered, causing callers such as
// PoolAllocator and static initializers (__GLOBAL__I_*) to fault at [x0, #0x10] (SIGSEGV at
// faultAddr 0x10) when reading the returned string reference's third word.
#include "runtime/cxx_forward.h"
#include "runtime/runtime.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace radeki::runtime {
namespace {

constexpr size_t kSSOCapacity = 22;  // libc++ char SSO capacity on 64-bit little-endian

// Apple's ABI, written out. A GuestStdString* is exactly what the guest passes as `this`.
struct GuestStdString {
  uint64_t w[3];

  bool isLong() const { return (w[0] & 1) != 0; }
  size_t size() const { return isLong() ? size_t(w[1]) : size_t((w[0] & 0xFF) >> 1); }
  size_t capacity() const { return isLong() ? size_t(w[0] >> 1) : kSSOCapacity; }
  char* data() { return isLong() ? reinterpret_cast<char*>(w[2]) : reinterpret_cast<char*>(this) + 1; }
  const char* data() const { return const_cast<GuestStdString*>(this)->data(); }

  void makeShort(const char* p, size_t n) {
    char* d = reinterpret_cast<char*>(this);
    std::memset(this, 0, sizeof *this);
    d[0] = static_cast<char>(n << 1);  // LSB clear == short mode
    if (n && p) std::memcpy(d + 1, p, n);
    d[1 + n] = 0;
  }
  bool makeLong(const char* p, size_t n, size_t cap) {
    size_t allocCap = (cap | 1);  // keep odd/aligned capacity so (cap<<1)|1 is clean
    if (allocCap < n) allocCap = n;
    char* buf = static_cast<char*>(std::malloc(allocCap + 1));
    if (!buf) return false;  // OOM: caller degrades to empty rather than aborting the host
    if (n && p) std::memcpy(buf, p, n);
    buf[n] = 0;
    w[0] = (uint64_t(allocCap) << 1) | 1;
    w[1] = n;
    w[2] = reinterpret_cast<uint64_t>(buf);
    return true;
  }
  void releaseLong() {
    if (isLong() && w[2] != 0) std::free(reinterpret_cast<void*>(w[2]));
  }
  // Ensure `this` is valid even if guest passed an uninitialized/zeroed struct.
  void sanitizeIfCorrupt() {
    if (isLong() && w[2] == 0) {
      makeShort("", 0);
    } else if (!isLong() && size() > kSSOCapacity) {
      makeShort("", 0);
    }
  }
};
static_assert(sizeof(GuestStdString) == 24, "libc++ basic_string<char> is 24 bytes on arm64");

// Static fallback empty string if a guest caller ever passes a null `this` pointer.
GuestStdString& fallbackGuestString() {
  static thread_local GuestStdString s = [] {
    GuestStdString init{};
    init.makeShort("", 0);
    return init;
  }();
  s.sanitizeIfCorrupt();
  return s;
}

GuestStdString* ensureValidString(GuestStdString* s) {
  if (!s) return &fallbackGuestString();
  s->sanitizeIfCorrupt();
  return s;
}

// __init semantics: initialize raw storage. Short input stays inline; long input gets a fresh
// allocation with capacity >= size.
void gsInit(GuestStdString* s, const char* p, size_t n) {
  s = ensureValidString(s);
  if (!p) { p = ""; n = 0; }  // defensive: a null C string becomes the empty string
  if (n <= kSSOCapacity) { s->makeShort(p, n); return; }
  if (!s->makeLong(p, n, n)) { s->makeShort("", 0); noteCompatCall("cxx_oom"); }
}

void gsFill(GuestStdString* s, size_t n, char c) {
  s = ensureValidString(s);
  if (n <= kSSOCapacity) {
    char tmp[kSSOCapacity];
    std::memset(tmp, c, n);
    s->makeShort(tmp, n);
    return;
  }
  size_t allocCap = (n | 1);
  char* buf = static_cast<char*>(std::malloc(allocCap + 1));
  if (!buf) { s->makeShort("", 0); noteCompatCall("cxx_oom"); return; }
  std::memset(buf, c, n);
  buf[n] = 0;
  s->w[0] = (uint64_t(allocCap) << 1) | 1;
  s->w[1] = n;
  s->w[2] = reinterpret_cast<uint64_t>(buf);
}

void gsDestroy(GuestStdString* s) {
  if (!s) return;
  s->sanitizeIfCorrupt();
  s->releaseLong();
  std::memset(s, 0, sizeof *s);
}

// Grow policy for append/insert: at least the required size, else roughly doubling.
size_t gsNextCapacity(size_t current, size_t required) {
  size_t cap = required;
  if (current >= kSSOCapacity) cap = std::max(cap, 2 * current + 1);
  else cap = std::max(cap, 2 * (kSSOCapacity + 1));
  return cap | 1;
}

void gsSetSize(GuestStdString* s, size_t n) {
  if (s->isLong()) s->w[1] = n;
  else reinterpret_cast<char*>(s)[0] = static_cast<char>(n << 1);
}

// General splice: replace [pos, pos + removeLen) with p[0, n).
// Preserves libc++'s grow-only storage policy and handles aliasing when p points inside s->data().
void gsSplice(GuestStdString* s, size_t pos, size_t removeLen, const char* p, size_t n) {
  s = ensureValidString(s);
  if (!p) { p = ""; n = 0; }
  size_t sz = s->size();
  if (pos > sz) pos = sz;
  if (removeLen > sz - pos) removeLen = sz - pos;
  size_t tail = sz - (pos + removeLen);
  size_t newSize = pos + n + tail;

  // Copy source bytes if `p` aliases into `s->data()` to stay safe across memmove/realloc.
  char* cur = s->data();
  std::vector<char> aliasCopy;
  if (n > 0 && p >= cur && p < cur + sz) {
    aliasCopy.assign(p, p + n);
    p = aliasCopy.data();
  }

  if (newSize <= s->capacity()) {
    std::memmove(cur + pos + n, cur + pos + removeLen, tail);
    if (n) std::memcpy(cur + pos, p, n);
    cur[newSize] = 0;
    gsSetSize(s, newSize);
    return;
  }
  size_t cap = gsNextCapacity(s->capacity(), newSize);
  char* buf = static_cast<char*>(std::malloc(cap + 1));
  if (!buf) { noteCompatCall("cxx_oom"); return; }  // keep the old contents
  if (pos) std::memcpy(buf, cur, pos);
  if (n) std::memcpy(buf + pos, p, n);
  if (tail) std::memcpy(buf + pos + n, cur + pos + removeLen, tail);
  buf[newSize] = 0;
  s->releaseLong();
  s->w[0] = (uint64_t(cap) << 1) | 1;
  s->w[1] = newSize;
  s->w[2] = reinterpret_cast<uint64_t>(buf);
}

void gsReplace(GuestStdString* s, size_t pos, const char* p, size_t n) {
  gsSplice(s, pos, 0, p, n);
}

// Replace-all (assign) semantics: overwrite [0, size) with p[0, n), reusing storage when it
// fits and growing (never shrinking) otherwise.
void gsAssign(GuestStdString* s, const char* p, size_t n) {
  s = ensureValidString(s);
  if (!p) { p = ""; n = 0; }
  if (n <= s->capacity()) {
    char* d = s->data();
    if (n) std::memmove(d, p, n);
    d[n] = 0;
    gsSetSize(s, n);
    return;
  }
  size_t cap = std::max(n, gsNextCapacity(s->capacity(), n));
  char* buf = static_cast<char*>(std::malloc(cap + 1));
  if (!buf) { noteCompatCall("cxx_oom"); return; }
  if (n) std::memcpy(buf, p, n);
  buf[n] = 0;
  s->releaseLong();
  s->w[0] = (uint64_t(cap) << 1) | 1;
  s->w[1] = n;
  s->w[2] = reinterpret_cast<uint64_t>(buf);
}

// --- wrappers with neutral names; the forwarding table registers them under Apple's ---------
// Note: All mutating methods that return `basic_string&` in C++ return `GuestStdString*` (`s`)
// so register x0 holds a valid, non-null guest string address on return.

// std::__1::basic_string<char>::__init(const char*, size_type)
GuestStdString* w_init_ptr_len(GuestStdString* s, const char* p, uint64_t n) {
  noteCompatCall("std::string::__init(const char*,size)");
  s = ensureValidString(s);
  gsInit(s, p, size_t(n));
  return s;
}
// std::__1::basic_string<char>::__init(const char*, size_type, size_type)
GuestStdString* w_init_ptr_len_cap(GuestStdString* s, const char* p, uint64_t n, uint64_t cap) {
  noteCompatCall("std::string::__init(const char*,size,cap)");
  s = ensureValidString(s);
  if (!p) { p = ""; n = 0; }
  size_t reqCap = std::max(size_t(n), size_t(cap));
  if (reqCap <= kSSOCapacity) {
    s->makeShort(p, size_t(n));
  } else if (!s->makeLong(p, size_t(n), reqCap)) {
    s->makeShort("", 0);
    noteCompatCall("cxx_oom");
  }
  return s;
}
// std::__1::basic_string<char>::__init(size_type, char)
GuestStdString* w_init_fill(GuestStdString* s, uint64_t n, char c) {
  noteCompatCall("std::string::__init(size,char)");
  s = ensureValidString(s);
  gsFill(s, size_t(n), c);
  return s;
}
// default constructors C1/C2: SSO-empty storage
GuestStdString* w_ctor_default(GuestStdString* s) {
  noteCompatCall("std::string::ctor()");
  s = ensureValidString(s);
  s->makeShort("", 0);
  return s;
}
// constructors from a C string, with and without the (ignored) allocator argument
GuestStdString* w_ctor_cstr(GuestStdString* s, const char* p) {
  noteCompatCall("std::string::ctor(const char*)");
  s = ensureValidString(s);
  gsInit(s, p, p ? std::strlen(p) : 0);
  return s;
}
GuestStdString* w_ctor_cstr_alloc(GuestStdString* s, const char* p, const void* /*alloc*/) {
  return w_ctor_cstr(s, p);
}
GuestStdString* w_ctor_ptr_len(GuestStdString* s, const char* p, uint64_t n) {
  noteCompatCall("std::string::ctor(const char*,size)");
  s = ensureValidString(s);
  gsInit(s, p, size_t(n));
  return s;
}
GuestStdString* w_ctor_fill(GuestStdString* s, uint64_t n, char c) {
  noteCompatCall("std::string::ctor(size,char)");
  s = ensureValidString(s);
  gsFill(s, size_t(n), c);
  return s;
}
// copy constructors C1/C2
GuestStdString* w_ctor_copy(GuestStdString* s, const GuestStdString* other) {
  noteCompatCall("std::string::ctor(const string&)");
  s = ensureValidString(s);
  if (other) {
    GuestStdString* o = const_cast<GuestStdString*>(other);
    o->sanitizeIfCorrupt();
    gsInit(s, o->data(), o->size());
  } else {
    s->makeShort("", 0);
  }
  return s;
}
// substring constructor: basic_string(const basic_string&, size_type pos, size_type n, const allocator&)
GuestStdString* w_ctor_substr(GuestStdString* s, const GuestStdString* other, uint64_t pos, uint64_t n, const void* /*alloc*/) {
  noteCompatCall("std::string::ctor(const string&,pos,n)");
  s = ensureValidString(s);
  if (!other) { s->makeShort("", 0); return s; }
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  size_t sz = o->size();
  size_t p = std::min(size_t(pos), sz);
  size_t count = std::min(size_t(n), sz - p);
  gsInit(s, o->data() + p, count);
  return s;
}
// destructors D1/D2
GuestStdString* w_dtor(GuestStdString* s) {
  noteCompatCall("std::string::dtor()");
  gsDestroy(s);
  return s;
}
// operator=(const basic_string&)
GuestStdString* w_assign_copy(GuestStdString* s, const GuestStdString* other) {
  noteCompatCall("std::string::operator=(const string&)");
  s = ensureValidString(s);
  if (!other) { gsAssign(s, "", 0); return s; }
  if (s == other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  gsAssign(s, o->data(), o->size());
  return s;
}
// operator=(const char*) and assign(const char*)
GuestStdString* w_assign_cstr(GuestStdString* s, const char* p) {
  noteCompatCall("std::string::assign(const char*)");
  s = ensureValidString(s);
  gsAssign(s, p, p ? std::strlen(p) : 0);
  return s;
}
// operator=(char)
GuestStdString* w_assign_char(GuestStdString* s, char c) {
  noteCompatCall("std::string::operator=(char)");
  s = ensureValidString(s);
  gsAssign(s, &c, 1);
  return s;
}
// assign(const char*, size_type)
GuestStdString* w_assign_ptr_len(GuestStdString* s, const char* p, uint64_t n) {
  noteCompatCall("std::string::assign(const char*,size)");
  s = ensureValidString(s);
  gsAssign(s, p, size_t(n));
  return s;
}
// assign(const basic_string&, size_type, size_type)
GuestStdString* w_assign_substr(GuestStdString* s, const GuestStdString* other, uint64_t pos, uint64_t n) {
  noteCompatCall("std::string::assign(const string&,pos,n)");
  s = ensureValidString(s);
  if (!other) { gsAssign(s, "", 0); return s; }
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  size_t sz = o->size();
  size_t p = std::min(size_t(pos), sz);
  size_t count = std::min(size_t(n), sz - p);
  gsAssign(s, o->data() + p, count);
  return s;
}
// assign(size_type, char)
GuestStdString* w_assign_fill(GuestStdString* s, uint64_t n, char c) {
  noteCompatCall("std::string::assign(size,char)");
  s = ensureValidString(s);
  if (size_t(n) <= s->capacity()) {
    char* d = s->data();
    std::memset(d, c, size_t(n));
    d[n] = 0;
    gsSetSize(s, size_t(n));
    return s;
  }
  s->releaseLong();
  gsFill(s, size_t(n), c);
  return s;
}
// append(const char*, size_type)
GuestStdString* w_append_ptr_len(GuestStdString* s, const char* p, uint64_t n) {
  noteCompatCall("std::string::append(const char*,size)");
  s = ensureValidString(s);
  gsReplace(s, s->size(), p, size_t(n));
  return s;
}
// append(const char*)
GuestStdString* w_append_cstr(GuestStdString* s, const char* p) {
  noteCompatCall("std::string::append(const char*)");
  s = ensureValidString(s);
  gsReplace(s, s->size(), p, p ? std::strlen(p) : 0);
  return s;
}
// append(const basic_string&)
GuestStdString* w_append_str(GuestStdString* s, const GuestStdString* other) {
  noteCompatCall("std::string::append(const string&)");
  s = ensureValidString(s);
  if (!other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  gsReplace(s, s->size(), o->data(), o->size());
  return s;
}
// append(const basic_string&, size_type, size_type)
GuestStdString* w_append_substr(GuestStdString* s, const GuestStdString* other, uint64_t pos, uint64_t n) {
  noteCompatCall("std::string::append(const string&,pos,n)");
  s = ensureValidString(s);
  if (!other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  size_t sz = o->size();
  size_t p = std::min(size_t(pos), sz);
  size_t count = std::min(size_t(n), sz - p);
  gsReplace(s, s->size(), o->data() + p, count);
  return s;
}
// append(size_type, char)
GuestStdString* w_append_fill(GuestStdString* s, uint64_t n, char c) {
  noteCompatCall("std::string::append(size,char)");
  s = ensureValidString(s);
  if (n == 0) return s;
  std::vector<char> fill(size_t(n), c);
  gsReplace(s, s->size(), fill.data(), size_t(n));
  return s;
}
// push_back(char)
void w_push_back(GuestStdString* s, char c) {
  noteCompatCall("std::string::push_back");
  s = ensureValidString(s);
  gsReplace(s, s->size(), &c, 1);
}
// pop_back()
void w_pop_back(GuestStdString* s) {
  noteCompatCall("std::string::pop_back");
  s = ensureValidString(s);
  size_t sz = s->size();
  if (sz > 0) {
    s->data()[sz - 1] = 0;
    gsSetSize(s, sz - 1);
  }
}
// clear()
void w_clear(GuestStdString* s) {
  noteCompatCall("std::string::clear");
  s = ensureValidString(s);
  s->data()[0] = 0;
  gsSetSize(s, 0);
}
// reserve(size_type): libc++ only grows here; reserve(n) with n <= capacity() is a no-op
// (it never collapses long storage back to SSO, keeping data() pointers stable).
void w_reserve(GuestStdString* s, uint64_t n) {
  noteCompatCall("std::string::reserve");
  s = ensureValidString(s);
  if (n <= s->capacity()) return;
  size_t sz = s->size();
  size_t cap = size_t(n) | 1;
  char* buf = static_cast<char*>(std::malloc(cap + 1));
  if (!buf) { noteCompatCall("cxx_oom"); return; }
  std::memcpy(buf, s->data(), sz);
  buf[sz] = 0;
  s->releaseLong();
  s->w[0] = (uint64_t(cap) << 1) | 1;
  s->w[1] = sz;
  s->w[2] = reinterpret_cast<uint64_t>(buf);
}
// resize(size_type, char) and resize(size_type)
void w_resize_fill(GuestStdString* s, uint64_t n, char c) {
  noteCompatCall("std::string::resize");
  s = ensureValidString(s);
  size_t sz = s->size();
  if (size_t(n) <= sz) {  // truncate in place; capacity is preserved, like libc++
    char* d = s->data();
    d[n] = 0;
    gsSetSize(s, size_t(n));
    return;
  }
  size_t extra = size_t(n) - sz;
  if (size_t(n) <= s->capacity()) {  // fits the existing storage (SSO or long)
    char* d = s->data();
    std::memset(d + sz, c, extra);
    d[n] = 0;
    gsSetSize(s, size_t(n));
    return;
  }
  std::vector<char> fill(extra, c);
  gsReplace(s, sz, fill.data(), extra);
}
void w_resize(GuestStdString* s, uint64_t n) { w_resize_fill(s, n, 0); }

// insert(size_type, const char*, size_type) -> returns basic_string& (this)
GuestStdString* w_insert_ptr_len(GuestStdString* s, uint64_t pos, const char* p, uint64_t n) {
  noteCompatCall("std::string::insert(size,const char*,size)");
  s = ensureValidString(s);
  gsReplace(s, size_t(pos), p, size_t(n));
  return s;
}
// insert(size_type, const char*) -> __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc
// Exact symbol that caused SIGSEGV at faultAddr: 0x10 during PoolAllocator / mod_init!
GuestStdString* w_insert_cstr(GuestStdString* s, uint64_t pos, const char* p) {
  noteCompatCall("std::string::insert(size,const char*)");
  s = ensureValidString(s);
  gsReplace(s, size_t(pos), p, p ? std::strlen(p) : 0);
  return s;
}
// insert(size_type, const basic_string&)
GuestStdString* w_insert_str(GuestStdString* s, uint64_t pos, const GuestStdString* other) {
  noteCompatCall("std::string::insert(size,const string&)");
  s = ensureValidString(s);
  if (!other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  gsReplace(s, size_t(pos), o->data(), o->size());
  return s;
}
// insert(size_type, const basic_string&, size_type, size_type)
GuestStdString* w_insert_substr(GuestStdString* s, uint64_t pos, const GuestStdString* other, uint64_t subpos, uint64_t sublen) {
  noteCompatCall("std::string::insert(size,const string&,size,size)");
  s = ensureValidString(s);
  if (!other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  size_t sz = o->size();
  size_t p = std::min(size_t(subpos), sz);
  size_t count = std::min(size_t(sublen), sz - p);
  gsReplace(s, size_t(pos), o->data() + p, count);
  return s;
}
// insert(size_type, size_type, char)
GuestStdString* w_insert_fill(GuestStdString* s, uint64_t pos, uint64_t n, char c) {
  noteCompatCall("std::string::insert(size,size,char)");
  s = ensureValidString(s);
  if (n == 0) return s;
  std::vector<char> fill(size_t(n), c);
  gsReplace(s, size_t(pos), fill.data(), size_t(n));
  return s;
}

// erase(size_type pos = 0, size_type n = npos)
GuestStdString* w_erase(GuestStdString* s, uint64_t pos, uint64_t n) {
  noteCompatCall("std::string::erase");
  s = ensureValidString(s);
  gsSplice(s, size_t(pos), size_t(n), "", 0);
  return s;
}

// replace(size_type pos, size_type len, const char* p, size_type n)
GuestStdString* w_replace_ptr_len(GuestStdString* s, uint64_t pos, uint64_t len, const char* p, uint64_t n) {
  noteCompatCall("std::string::replace(pos,len,const char*,n)");
  s = ensureValidString(s);
  gsSplice(s, size_t(pos), size_t(len), p, size_t(n));
  return s;
}
// replace(size_type pos, size_type len, const char* p)
GuestStdString* w_replace_cstr(GuestStdString* s, uint64_t pos, uint64_t len, const char* p) {
  noteCompatCall("std::string::replace(pos,len,const char*)");
  s = ensureValidString(s);
  gsSplice(s, size_t(pos), size_t(len), p, p ? std::strlen(p) : 0);
  return s;
}
// replace(size_type pos, size_type len, const basic_string& str)
GuestStdString* w_replace_str(GuestStdString* s, uint64_t pos, uint64_t len, const GuestStdString* other) {
  noteCompatCall("std::string::replace(pos,len,const string&)");
  s = ensureValidString(s);
  if (!other) return s;
  GuestStdString* o = const_cast<GuestStdString*>(other);
  o->sanitizeIfCorrupt();
  gsSplice(s, size_t(pos), size_t(len), o->data(), o->size());
  return s;
}
// replace(size_type pos, size_type len, size_type n, char c)
GuestStdString* w_replace_fill(GuestStdString* s, uint64_t pos, uint64_t len, uint64_t n, char c) {
  noteCompatCall("std::string::replace(pos,len,n,char)");
  s = ensureValidString(s);
  std::vector<char> fill(size_t(n), c);
  gsSplice(s, size_t(pos), size_t(len), fill.data(), size_t(n));
  return s;
}

// libc++ internal helper: __grow_by_and_replace(old_cap, delta_cap, old_sz, n_copy, n_del, n_add, p_new_stuff)
void w_grow_by_and_replace(GuestStdString* s, uint64_t /*old_cap*/, uint64_t /*delta_cap*/,
                           uint64_t /*old_sz*/, uint64_t n_copy, uint64_t n_del,
                           uint64_t n_add, const char* p_new_stuff) {
  noteCompatCall("std::string::__grow_by_and_replace");
  s = ensureValidString(s);
  gsSplice(s, size_t(n_copy), size_t(n_del), p_new_stuff, size_t(n_add));
}

// libc++ internal helper: __grow_by(old_cap, delta_cap, old_sz, n_copy, n_del, n_add)
void w_grow_by(GuestStdString* s, uint64_t old_cap, uint64_t delta_cap,
               uint64_t old_sz, uint64_t n_copy, uint64_t n_del, uint64_t n_add) {
  noteCompatCall("std::string::__grow_by");
  s = ensureValidString(s);
  size_t newCap = std::max(size_t(old_cap + delta_cap), size_t(old_sz - n_del + n_add)) | 1;
  char* buf = static_cast<char*>(std::malloc(newCap + 1));
  if (!buf) { noteCompatCall("cxx_oom"); return; }
  char* cur = s->data();
  size_t copyHead = std::min(size_t(n_copy), size_t(old_sz));
  if (copyHead) std::memcpy(buf, cur, copyHead);
  size_t n_sec = (old_sz > n_del + n_copy) ? size_t(old_sz - n_del - n_copy) : 0;
  if (n_sec) std::memcpy(buf + copyHead + n_add, cur + copyHead + n_del, n_sec);
  s->releaseLong();
  s->w[0] = (uint64_t(newCap) << 1) | 1;
  s->w[2] = reinterpret_cast<uint64_t>(buf);
}

// compare(const char*) const
int w_compare_cstr(const GuestStdString* s, const char* rhs) {
  noteCompatCall("std::string::compare(const char*)");
  if (!rhs) rhs = "";
  size_t sz = s ? s->size() : 0;
  const char* lhs = s ? s->data() : "";
  size_t rl = std::strlen(rhs);
  size_t n = std::min(sz, rl);
  int c = n ? std::memcmp(lhs, rhs, n) : 0;
  if (c) return c;
  return sz < rl ? -1 : (sz > rl ? 1 : 0);
}
// compare(size_type, size_type, const char*, size_type) const
int w_compare_sub_ptr_len(const GuestStdString* s, uint64_t pos1, uint64_t n1, const char* rhs, uint64_t n2) {
  noteCompatCall("std::string::compare(pos,n1,const char*,n2)");
  if (!rhs) { rhs = ""; n2 = 0; }
  size_t sz = s ? s->size() : 0;
  const char* lhs = s ? s->data() : "";
  size_t p1 = std::min(size_t(pos1), sz);
  size_t len1 = std::min(size_t(n1), sz - p1);
  size_t len2 = size_t(n2);
  size_t n = std::min(len1, len2);
  int c = n ? std::memcmp(lhs + p1, rhs, n) : 0;
  if (c) return c;
  return len1 < len2 ? -1 : (len1 > len2 ? 1 : 0);
}
// compare(size_type, size_type, const char*) const
int w_compare_sub_cstr(const GuestStdString* s, uint64_t pos1, uint64_t n1, const char* rhs) {
  return w_compare_sub_ptr_len(s, pos1, n1, rhs, rhs ? std::strlen(rhs) : 0);
}

// find(char, size_type) const
uint64_t w_find_char(const GuestStdString* s, char c, uint64_t pos) {
  noteCompatCall("std::string::find(char,size)");
  if (!s) return ~uint64_t(0);
  size_t sz = s->size();
  if (pos >= sz) return ~uint64_t(0);
  const char* d = s->data();
  const void* p = std::memchr(d + pos, static_cast<unsigned char>(c), sz - size_t(pos));
  return p ? static_cast<uint64_t>( static_cast<const char*>(p) - d ) : ~uint64_t(0);
}
// find(const char*, size_type, size_type) const
uint64_t w_find_ptr_pos_len(const GuestStdString* s, const char* needle, uint64_t pos, uint64_t n) {
  noteCompatCall("std::string::find(const char*,pos,n)");
  if (!s) return ~uint64_t(0);
  size_t sz = s->size();
  if (!needle || n == 0) return pos <= sz ? pos : ~uint64_t(0);
  if (pos > sz || n > sz - pos) return ~uint64_t(0);
  const char* d = s->data();
  for (size_t i = size_t(pos); i + n <= sz; ++i) {
    if (std::memcmp(d + i, needle, size_t(n)) == 0) return i;
  }
  return ~uint64_t(0);
}
// rfind(char, size_type) const
uint64_t w_rfind_char(const GuestStdString* s, char c, uint64_t pos) {
  noteCompatCall("std::string::rfind(char,size)");
  if (!s) return ~uint64_t(0);
  size_t sz = s->size();
  if (sz == 0) return ~uint64_t(0);
  size_t i = std::min(size_t(pos), sz - 1);
  const char* d = s->data();
  for (;; --i) {
    if (d[i] == c) return i;
    if (i == 0) break;
  }
  return ~uint64_t(0);
}
// rfind(const char*, size_type, size_type) const
uint64_t w_rfind_ptr_pos_len(const GuestStdString* s, const char* needle, uint64_t pos, uint64_t n) {
  noteCompatCall("std::string::rfind(const char*,pos,n)");
  if (!s) return ~uint64_t(0);
  size_t sz = s->size();
  if (!needle || n == 0) return std::min(size_t(pos), sz);
  if (n > sz) return ~uint64_t(0);
  size_t start = std::min(size_t(pos), sz - size_t(n));
  const char* d = s->data();
  for (size_t i = start;; --i) {
    if (std::memcmp(d + i, needle, size_t(n)) == 0) return i;
    if (i == 0) break;
  }
  return ~uint64_t(0);
}

// at(size_type)
char* w_at(GuestStdString* s, uint64_t pos) {
  noteCompatCall("std::string::at");
  s = ensureValidString(s);
  size_t sz = s->size();
  if (pos >= sz) return s->data();
  return s->data() + pos;
}
const char* w_at_const(const GuestStdString* s, uint64_t pos) {
  return w_at(const_cast<GuestStdString*>(s), pos);
}

// operator+(const char*, const basic_string&) and similar returning GuestStdString by value (via x8 indirect result on AArch64).
// Note: In AAPCS64, non-trivial C++ structs (like basic_string, which has a non-trivial dtor/copy-ctor)
// are returned via the indirect result location register x8 (or on some ABIs x0).
// However, libc++ marks operator+ inline and calls insert/append on the member functions above!

// std::__1::ios_base::Init -- the classic C++ static-init dependency. Its only job is to
// exist so __GLOBAL__I_* constructors complete; iostream formatting itself reaches us only
// through the stub dispatcher.
void* w_ios_init_ctor(void* self) { noteCompatCall("std::ios_base::Init::Init"); return self; }
void* w_ios_init_dtor(void* self) { noteCompatCall("std::ios_base::Init::~Init"); return self; }

// libc++'s __hash_table calls this exported helper when it grows its bucket array. A logging
// no-op return of zero is not safe here: rehash then leaves an unusable bucket count and the
// next insert dereferences invalid table storage (the ChunkPos crash reported by MCPE).
// Preserve libc++'s n==0 empty-table sentinel; otherwise return the smallest prime >= the
// requested bucket count. Deterministic Miller-Rabin keeps pathological size_t inputs bounded
// instead of allowing a trial-division shim to spend billions of iterations on a large request.
using WideUInt = unsigned __int128;

uint64_t multiplyModulo(uint64_t a, uint64_t b, uint64_t modulus) {
  return static_cast<uint64_t>((static_cast<WideUInt>(a) * b) % modulus);
}

uint64_t powerModulo(uint64_t base, uint64_t exponent, uint64_t modulus) {
  uint64_t result = 1;
  while (exponent) {
    if (exponent & 1) result = multiplyModulo(result, base, modulus);
    exponent >>= 1;
    if (exponent) base = multiplyModulo(base, base, modulus);
  }
  return result;
}

bool isPrime64(uint64_t n) {
  if (n < 2) return false;
  static constexpr uint32_t smallPrimes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
  for (uint32_t p : smallPrimes) {
    if (n % p == 0) return n == p;
  }

  uint64_t oddPart = n - 1;
  unsigned int powersOfTwo = 0;
  while ((oddPart & 1) == 0) {
    oddPart >>= 1;
    ++powersOfTwo;
  }
  // This base set is deterministic for every unsigned 64-bit integer.
  static constexpr uint64_t witnesses[] = {2, 325, 9375, 28178, 450775, 9780504, 1795265022};
  for (uint64_t witness : witnesses) {
    if (witness % n == 0) continue;
    uint64_t x = powerModulo(witness % n, oddPart, n);
    if (x == 1 || x == n - 1) continue;
    bool reachedMinusOne = false;
    for (unsigned int r = 1; r < powersOfTwo; ++r) {
      x = multiplyModulo(x, x, n);
      if (x == n - 1) {
        reachedMinusOne = true;
        break;
      }
    }
    if (!reachedMinusOne) return false;
  }
  return true;
}

size_t w_next_prime(size_t requested) {
  noteCompatCall("std::__next_prime");
  if (requested == 0) return 0;  // libc++ explicitly preserves the empty-table case
  if (requested <= 2) return 2;

  const size_t max = std::numeric_limits<size_t>::max();
  size_t candidate = requested;
  if ((candidate & 1) == 0) ++candidate;
  for (;;) {
    if (isPrime64(static_cast<uint64_t>(candidate))) return candidate;
    if (candidate > max - 2) return max;  // no representable larger odd candidate
    candidate += 2;
  }
}

template <class F> uint64_t fn(F* f) { return reinterpret_cast<uint64_t>(f); }

// basic_string data symbol: std::string::npos lives in the dylib on iOS.
const uint64_t kGuestNpos = ~uint64_t(0);

constexpr const char* S = "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
constexpr const char* SC = "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";

}  // namespace

const std::vector<CxxForwardEntry>& cxxForwardTable() {
  static const std::vector<CxxForwardEntry> real = [] {
    std::vector<CxxForwardEntry> t;
    auto add = [&](const std::string& m, uint64_t impl, const char* what, bool data = false) {
      t.push_back({m, impl, what, data});
    };
    // __init overloads
    add(std::string(S) + "6__initEPKcm", fn(w_init_ptr_len), "std::string::__init(const char*, size_type)");
    add(std::string(S) + "6__initEPKcmm", fn(w_init_ptr_len_cap), "std::string::__init(const char*, size_type, size_type)");
    add(std::string(S) + "6__initEmc", fn(w_init_fill), "std::string::__init(size_type, char)");
    // Constructors & destructors
    add(std::string(S) + "C1Ev", fn(w_ctor_default), "std::string::string()");
    add(std::string(S) + "C2Ev", fn(w_ctor_default), "std::string::string() [base]");
    add(std::string(S) + "C1EPKc", fn(w_ctor_cstr), "std::string::string(const char*)");
    add(std::string(S) + "C2EPKc", fn(w_ctor_cstr), "std::string::string(const char*) [base]");
    add(std::string(S) + "C1EPKcRKS4_", fn(w_ctor_cstr_alloc), "std::string::string(const char*, const allocator&)");
    add(std::string(S) + "C2EPKcRKS4_", fn(w_ctor_cstr_alloc), "std::string::string(const char*, const allocator&) [base]");
    add(std::string(S) + "C1EPKcm", fn(w_ctor_ptr_len), "std::string::string(const char*, size_type)");
    add(std::string(S) + "C2EPKcm", fn(w_ctor_ptr_len), "std::string::string(const char*, size_type) [base]");
    add(std::string(S) + "C1Emc", fn(w_ctor_fill), "std::string::string(size_type, char)");
    add(std::string(S) + "C2Emc", fn(w_ctor_fill), "std::string::string(size_type, char) [base]");
    add(std::string(S) + "C1ERKS5_", fn(w_ctor_copy), "std::string::string(const string&)");
    add(std::string(S) + "C2ERKS5_", fn(w_ctor_copy), "std::string::string(const string&) [base]");
    add(std::string(S) + "C1ERKS5_mmRKS4_", fn(w_ctor_substr), "std::string::string(const string&, size_type, size_type, const allocator&)");
    add(std::string(S) + "C2ERKS5_mmRKS4_", fn(w_ctor_substr), "std::string::string(const string&, size_type, size_type, const allocator&) [base]");
    add(std::string(S) + "D1Ev", fn(w_dtor), "std::string::~string()");
    add(std::string(S) + "D2Ev", fn(w_dtor), "std::string::~string() [base]");
    // Assignment & assign
    add(std::string(S) + "aSERKS5_", fn(w_assign_copy), "std::string::operator=(const string&)");
    add(std::string(S) + "aSEPKc", fn(w_assign_cstr), "std::string::operator=(const char*)");
    add(std::string(S) + "aSEc", fn(w_assign_char), "std::string::operator=(char)");
    add(std::string(S) + "6assignEPKc", fn(w_assign_cstr), "std::string::assign(const char*)");
    add(std::string(S) + "6assignEPKcm", fn(w_assign_ptr_len), "std::string::assign(const char*, size_type)");
    add(std::string(S) + "6assignERKS5_", fn(w_assign_copy), "std::string::assign(const string&)");
    add(std::string(S) + "6assignERKS5_mm", fn(w_assign_substr), "std::string::assign(const string&, size_type, size_type)");
    add(std::string(S) + "6assignEmc", fn(w_assign_fill), "std::string::assign(size_type, char)");
    // Append & push_back
    add(std::string(S) + "6appendEPKc", fn(w_append_cstr), "std::string::append(const char*)");
    add(std::string(S) + "6appendEPKcm", fn(w_append_ptr_len), "std::string::append(const char*, size_type)");
    add(std::string(S) + "6appendERKS5_", fn(w_append_str), "std::string::append(const string&)");
    add(std::string(S) + "6appendERKS5_mm", fn(w_append_substr), "std::string::append(const string&, size_type, size_type)");
    add(std::string(S) + "6appendEmc", fn(w_append_fill), "std::string::append(size_type, char)");
    add(std::string(S) + "9push_backEc", fn(w_push_back), "std::string::push_back(char)");
    add(std::string(S) + "8pop_backEv", fn(w_pop_back), "std::string::pop_back()");
    add(std::string(S) + "5clearEv", fn(w_clear), "std::string::clear()");
    // Reserve & resize
    add(std::string(S) + "7reserveEm", fn(w_reserve), "std::string::reserve(size_type)");
    add(std::string(S) + "6resizeEm", fn(w_resize), "std::string::resize(size_type)");
    add(std::string(S) + "6resizeEmc", fn(w_resize_fill), "std::string::resize(size_type, char)");
    // Insert overloads (including 6insertEmPKc which resolved the PoolAllocator faultAddr: 0x10 crash)
    add(std::string(S) + "6insertEmPKc", fn(w_insert_cstr), "std::string::insert(size_type, const char*)");
    add(std::string(S) + "6insertEmPKcm", fn(w_insert_ptr_len), "std::string::insert(size_type, const char*, size_type)");
    add(std::string(S) + "6insertEmRKS5_", fn(w_insert_str), "std::string::insert(size_type, const string&)");
    add(std::string(S) + "6insertEmRKS5_mm", fn(w_insert_substr), "std::string::insert(size_type, const string&, size_type, size_type)");
    add(std::string(S) + "6insertEmmc", fn(w_insert_fill), "std::string::insert(size_type, size_type, char)");
    // Erase, replace & internal grow helpers
    add(std::string(S) + "5eraseEmm", fn(w_erase), "std::string::erase(size_type, size_type)");
    add(std::string(S) + "7replaceEmmPKc", fn(w_replace_cstr), "std::string::replace(size_type, size_type, const char*)");
    add(std::string(S) + "7replaceEmmPKcm", fn(w_replace_ptr_len), "std::string::replace(size_type, size_type, const char*, size_type)");
    add(std::string(S) + "7replaceEmmRKS5_", fn(w_replace_str), "std::string::replace(size_type, size_type, const string&)");
    add(std::string(S) + "7replaceEmmmmc", fn(w_replace_fill), "std::string::replace(size_type, size_type, size_type, char)");
    add(std::string(S) + "21__grow_by_and_replaceEmmmmmmPKc", fn(w_grow_by_and_replace), "std::string::__grow_by_and_replace");
    add(std::string(S) + "9__grow_byEmmmmmm", fn(w_grow_by), "std::string::__grow_by");
    // Access & search (both non-const S and const SC manglings)
    add(std::string(S) + "2atEm", fn(w_at), "std::string::at(size_type)");
    add(std::string(SC) + "2atEm", fn(w_at_const), "std::string::at(size_type) const");
    add(std::string(S) + "6compareEPKc", fn(w_compare_cstr), "std::string::compare(const char*)");
    add(std::string(SC) + "7compareEPKc", fn(w_compare_cstr), "std::string::compare(const char*) const");
    add(std::string(SC) + "7compareEmmPKc", fn(w_compare_sub_cstr), "std::string::compare(size_type, size_type, const char*) const");
    add(std::string(SC) + "7compareEmmPKcm", fn(w_compare_sub_ptr_len), "std::string::compare(size_type, size_type, const char*, size_type) const");
    add(std::string(SC) + "4findEcm", fn(w_find_char), "std::string::find(char, size_type) const");
    add(std::string(SC) + "4findEPKcmm", fn(w_find_ptr_pos_len), "std::string::find(const char*, size_type, size_type) const");
    add(std::string(SC) + "5rfindEcm", fn(w_rfind_char), "std::string::rfind(char, size_type) const");
    add(std::string(SC) + "5rfindEPKcmm", fn(w_rfind_ptr_pos_len), "std::string::rfind(const char*, size_type, size_type) const");
    add(std::string(S) + "4nposE", reinterpret_cast<uint64_t>(&kGuestNpos), "std::string::npos", true);
    // libc++ hash-table support: the Minecraft PE ChunkPos unordered-set crash reached this
    // import after the generic no-op returned zero during __hash_table::__insert_unique.
    add("__ZNSt3__112__next_primeEm", fn(w_next_prime), "std::__1::__next_prime(size_t)");
    // ios_base::Init: required by any TU that touches <iostream> at static-init time.
    add("__ZNSt3__18ios_base4InitC1Ev", fn(w_ios_init_ctor), "std::ios_base::Init::Init()");
    add("__ZNSt3__18ios_base4InitC2Ev", fn(w_ios_init_ctor), "std::ios_base::Init::Init() [base]");
    add("__ZNSt3__18ios_base4InitD1Ev", fn(w_ios_init_dtor), "std::ios_base::Init::~Init()");
    add("__ZNSt3__18ios_base4InitD2Ev", fn(w_ios_init_dtor), "std::ios_base::Init::~Init() [base]");
    return t;
  }();
  return real;
}

std::optional<uint64_t> lookupCxxForward(const std::string& mangled) {
  for (const auto& e : cxxForwardTable())
    if (mangled == e.mangled) return e.impl;
  return std::nullopt;
}

void addCxxForwarding(relinker::CompatRegistry& reg) {
  for (const auto& e : cxxForwardTable())
    reg.add(e.mangled, e.impl, {compat::SymbolClass::AndroidBackend, "libc++",
                                 std::string("libc++_shared_forward: ") + e.what});
}

bool isAppleCxxDylib(const std::string& installName) {
  return installName.find("libc++") != std::string::npos ||
         installName.find("libc++abi") != std::string::npos;
}

// --- dlsym pass-through --------------------------------------------------------------------

struct HostCxxResolver::Cache {
  std::mutex mu;
  std::unordered_map<std::string, std::optional<uint64_t>> entries;
};

HostCxxResolver::Cache& HostCxxResolver::cache() {
  static Cache c;
  return c;
}

std::optional<uint64_t> HostCxxResolver::resolve(const std::string& symbol, const std::string& dylib) const {
  // Only C++ mangled names (__Z on Mach-O, _Z on ELF), and only when the IPA asked an Apple
  // C++ dylib for them (or asked flat). Android's libc++ uses the identical std::__1 mangling,
  // so a hit is ABI-compatible.
  bool machOMangled = symbol.rfind("__Z", 0) == 0;
  bool elfMangled = symbol.rfind("_Z", 0) == 0;
  if (!machOMangled && !elfMangled) return std::nullopt;
  if (!dylib.empty() && !isAppleCxxDylib(dylib)) return std::nullopt;
  // Never resolve exception/unwinding machinery via dlsym.
  if (symbol.find("cxa_throw") != std::string::npos ||
      symbol.find("cxa_begin_catch") != std::string::npos ||
      symbol.find("Unwind") != std::string::npos) {
    return std::nullopt;
  }
  Cache& c = cache();
  {
    std::lock_guard<std::mutex> lock(c.mu);
    auto it = c.entries.find(symbol);
    if (it != c.entries.end()) return it->second;
  }
  const std::string elfName = machOMangled ? symbol.substr(1) : symbol;
  void* p = dlsym(RTLD_DEFAULT, elfName.c_str());
  if (!p && machOMangled) p = dlsym(RTLD_DEFAULT, symbol.c_str());
  std::optional<uint64_t> hit;
  if (p) hit = reinterpret_cast<uint64_t>(p);
  std::lock_guard<std::mutex> lock(c.mu);
  c.entries[symbol] = hit;
  return hit;
}

std::optional<compat::CompatEntry> HostCxxResolver::describe(const std::string& symbol, const std::string& dylib) const {
  if (!resolve(symbol, dylib)) return std::nullopt;
  return compat::CompatEntry{compat::SymbolClass::AndroidBackend, "libc++", "dlsym_forward"};
}

size_t HostCxxResolver::cachedHits() const {
  Cache& c = cache();
  std::lock_guard<std::mutex> lock(c.mu);
  size_t n = 0;
  for (const auto& kv : c.entries) n += kv.second.has_value();
  return n;
}

}  // namespace radeki::runtime
