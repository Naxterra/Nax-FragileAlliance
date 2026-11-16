// Reads and writes PNG pictures through the Windows Imaging Component, for the texture dumps and packs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace nax {

// Pixels are 32-bit RGBA with red in the lowest byte and straight (not premultiplied) alpha.
bool save_png(const std::filesystem::path& file, const uint32_t* rgba, int width, int height);
bool load_png(const std::filesystem::path& file, std::vector<uint32_t>* rgba, int* width, int* height);

}  // namespace nax
