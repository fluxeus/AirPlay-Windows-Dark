#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ap::audio {
inline std::optional<uint32_t> flush_timestamp(std::string_view info) {
    std::size_t pos = 0;
    while (pos < info.size()) {
        const auto end = info.find_first_of(";,", pos);
        auto field = info.substr(pos, end == std::string_view::npos ? info.size() - pos : end - pos);
        while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
        while (!field.empty() && field.back() == ' ') field.remove_suffix(1);
        constexpr std::string_view prefix = "rtptime=";
        if (field.substr(0, prefix.size()) == prefix) {
            field.remove_prefix(prefix.size());
            uint32_t value = 0;
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size()) return value;
            return {};
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return {};
}
// Drop samples strictly before the RTP boundary, including partial PCM blocks.
inline std::size_t trim_before(uint32_t& rtp, std::vector<int16_t>& pcm, uint32_t boundary) {
    const uint32_t difference = boundary - rtp;
    if (difference == 0 || difference > 0x7fffffffU) return 0;
    const auto frames = std::min<std::size_t>(difference, pcm.size() / 2);
    pcm.erase(pcm.begin(), pcm.begin() + frames * 2);
    rtp += static_cast<uint32_t>(frames);
    return frames * 2;
}
}
