#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dat_player::playback {

struct BgraVideoFrame {
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;
    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    std::uint32_t stride_bytes = 0;
    std::wstring decoded_subtype;
    std::vector<std::uint8_t> pixels;
};

} // namespace dat_player::playback
