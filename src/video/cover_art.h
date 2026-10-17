#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ap::video {
// Tightly packed RGB24 output, owned by the caller. FFmpeg conversion uses
// separately allocated, aligned and padded image planes.
bool decode_cover_art(const uint8_t* data, std::size_t size,
                      int& width, int& height, std::vector<uint8_t>& rgb);
}
