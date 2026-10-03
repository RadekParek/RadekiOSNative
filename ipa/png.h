#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace radeki::ipa {
struct PngInfo { uint32_t width=0,height=0; uint8_t bitDepth=0,colorType=0; bool cgbi=false; };
struct RgbaImage { uint32_t width=0,height=0; std::vector<uint8_t> rgba; };
bool pngInfo(const std::vector<uint8_t>& bytes, PngInfo& info, std::string& error);
bool decodePng(const std::vector<uint8_t>& bytes, RgbaImage& image, std::string& error);
std::vector<uint8_t> encodePng(const RgbaImage& image);
}
