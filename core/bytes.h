// Bounds-checked byte access. Every read of untrusted data goes through Reader.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

static_assert(std::endian::native == std::endian::little, "host must be little-endian");

namespace radeki {

struct FormatError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Bytes = std::span<const uint8_t>;

class Reader {
 public:
  Reader() = default;
  explicit Reader(Bytes d) : d_(d) {}
  Bytes bytes() const { return d_; }
  uint64_t size() const { return d_.size(); }

  bool inRange(uint64_t off, uint64_t len) const {
    uint64_t end;
    return !__builtin_add_overflow(off, len, &end) && end <= d_.size();
  }
  void need(uint64_t off, uint64_t len, const char* what) const {
    if (!inRange(off, len)) throw FormatError(std::string("out of bounds: ") + what);
  }
  template <class T>
  T read(uint64_t off, const char* what = "read") const {
    need(off, sizeof(T), what);
    T v;
    std::memcpy(&v, d_.data() + off, sizeof(T));
    return v;
  }
  uint32_t be32(uint64_t off, const char* what = "be32") const {
    return __builtin_bswap32(read<uint32_t>(off, what));
  }
  uint64_t be64(uint64_t off, const char* what = "be64") const {
    return __builtin_bswap64(read<uint64_t>(off, what));
  }
  Reader sub(uint64_t off, uint64_t len, const char* what) const {
    need(off, len, what);
    return Reader(d_.subspan(off, len));
  }
  // NUL-terminated string that must end before `limit` (clamped to size).
  std::string cstr(uint64_t off, uint64_t limit, const char* what) const {
    if (limit > d_.size()) limit = d_.size();
    if (off >= limit) throw FormatError(std::string("string out of bounds: ") + what);
    const uint8_t* p = d_.data() + off;
    uint64_t n = limit - off;
    if (n > 65536) n = 65536;
    const void* z = std::memchr(p, 0, n);
    if (!z) throw FormatError(std::string("unterminated string: ") + what);
    return std::string(reinterpret_cast<const char*>(p), static_cast<const uint8_t*>(z) - p);
  }
  // Fixed-width name (segment/section names), not necessarily terminated.
  std::string fixedName(uint64_t off, size_t n, const char* what) const {
    need(off, n, what);
    const char* p = reinterpret_cast<const char*>(d_.data() + off);
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(p, len);
  }
  uint64_t uleb(uint64_t& off, uint64_t end, const char* what) const {
    uint64_t result = 0;
    unsigned shift = 0;
    for (;;) {
      if (off >= end || !inRange(off, 1)) throw FormatError(std::string("truncated ULEB: ") + what);
      uint8_t b = d_[off++];
      if (shift < 64) result |= uint64_t(b & 0x7F) << shift;
      else if (b & 0x7F) throw FormatError(std::string("ULEB overflow: ") + what);
      if (!(b & 0x80)) return result;
      shift += 7;
      if (shift > 70) throw FormatError(std::string("ULEB too long: ") + what);
    }
  }
  int64_t sleb(uint64_t& off, uint64_t end, const char* what) const {
    int64_t result = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
      if (off >= end || !inRange(off, 1)) throw FormatError(std::string("truncated SLEB: ") + what);
      b = d_[off++];
      if (shift < 64) result |= int64_t(uint64_t(b & 0x7F) << shift);
      shift += 7;
      if (shift > 70) throw FormatError(std::string("SLEB too long: ") + what);
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) result |= -(int64_t(1) << shift);
    return result;
  }

 private:
  Bytes d_;
};

inline uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace radeki
