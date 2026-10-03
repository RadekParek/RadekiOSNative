#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace radeki::ipa {
struct Plist {
  enum class Type { Null, Bool, Integer, Real, String, Array, Dict, Data, Date, Uid };
  Type type=Type::Null;
  bool boolean=false;
  int64_t integer=0;
  double real=0;
  std::string text;
  std::vector<Plist> array;
  std::map<std::string,Plist> dict;
  std::vector<uint8_t> data;
  const Plist* find(const std::string& key) const;
  const Plist* path(const std::string& dotted) const;
  std::string stringOr(const std::string& fallback="") const;
  int64_t intOr(int64_t fallback=0) const;
  std::vector<std::string> stringsOr() const;
};
bool parsePlist(const std::vector<uint8_t>& bytes,Plist& out,std::string& error);
}
