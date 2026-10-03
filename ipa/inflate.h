#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace radeki::ipa {
uint32_t crc32(const uint8_t*, size_t);
uint32_t adler32(const uint8_t*, size_t);
bool inflateRaw(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t limit, std::string& error);
bool zlibInflate(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t limit, std::string& error);
std::vector<uint8_t> zlibDeflateStored(const std::vector<uint8_t>& input);
}
