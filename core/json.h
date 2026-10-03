// Minimal streaming JSON writer for machine-readable CLI output.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace radeki {

class JsonWriter {
 public:
  JsonWriter& beginObject() { sep(); s_ += '{'; stack_.push_back(true); return *this; }
  JsonWriter& endObject() { s_ += '}'; stack_.pop_back(); return *this; }
  JsonWriter& beginArray() { sep(); s_ += '['; stack_.push_back(true); return *this; }
  JsonWriter& endArray() { s_ += ']'; stack_.pop_back(); return *this; }
  JsonWriter& key(std::string_view k) { sep(); quote(k); s_ += ':'; afterKey_ = true; return *this; }
  JsonWriter& str(std::string_view v) { sep(); quote(v); return *this; }
  JsonWriter& num(int64_t v) { sep(); s_ += std::to_string(v); return *this; }
  JsonWriter& unum(uint64_t v) { sep(); s_ += std::to_string(v); return *this; }
  JsonWriter& boolean(bool v) { sep(); s_ += v ? "true" : "false"; return *this; }
  JsonWriter& hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return str(b);
  }
  JsonWriter& kv(std::string_view k, std::string_view v) { return key(k).str(v); }
  JsonWriter& kv(std::string_view k, const char* v) { return key(k).str(v); }
  JsonWriter& kvu(std::string_view k, uint64_t v) { return key(k).unum(v); }
  JsonWriter& kvb(std::string_view k, bool v) { return key(k).boolean(v); }
  JsonWriter& kvh(std::string_view k, uint64_t v) { return key(k).hex(v); }
  const std::string& str() const { return s_; }

 private:
  void sep() {
    if (afterKey_) { afterKey_ = false; return; }
    if (stack_.empty()) return;
    if (!stack_.back()) s_ += ',';
    stack_.back() = false;
  }
  void quote(std::string_view v) {
    s_ += '"';
    for (unsigned char c : v) {
      switch (c) {
        case '"': s_ += "\\\""; break;
        case '\\': s_ += "\\\\"; break;
        case '\n': s_ += "\\n"; break;
        case '\r': s_ += "\\r"; break;
        case '\t': s_ += "\\t"; break;
        default:
          if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); s_ += b; }
          else s_ += char(c);
      }
    }
    s_ += '"';
  }
  std::string s_;
  std::vector<bool> stack_;
  bool afterKey_ = false;
};

}  // namespace radeki
