#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <vector>

#include "mach_o/macho.h"

using namespace radeki::macho;

static std::vector<uint8_t> slurp(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(std::string("cannot open ") + path);
  return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: dump_sections <macho>\n");
    return 2;
  }
  try {
    auto bytes = slurp(argv[1]);
    auto slices = listSlices(bytes);
    auto choice = chooseSlice(slices);
    if (!choice.index) throw std::runtime_error(choice.reason);
    Image image = parseSlice(bytes, slices[*choice.index]);
    std::printf("arch %s ptrSize %u classicBinds %s\n", archName(image.arch),
                static_cast<unsigned>(image.ptrSize()), image.classicBinds ? "yes" : "no");
    for (const auto& segment : image.segments)
      for (const auto& section : segment.sections)
        std::printf("%-16s %-24s addr=0x%llx size=0x%llx type=%u reserved1=%u reserved2=%u\n",
                    segment.name.c_str(), section.name.c_str(),
                    static_cast<unsigned long long>(section.addr),
                    static_cast<unsigned long long>(section.size), section.type(),
                    section.reserved1, section.reserved2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "dump_sections: %s\n", error.what());
    return 1;
  }
}
