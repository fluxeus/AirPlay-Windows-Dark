#pragma once

#include <string>

namespace ap::airplay {

// RAOP prepends 12 MAC digits and '@' to the DNS-SD label (63 bytes).
// Return an empty string for an invalid name; never truncate UTF-8.
inline std::string normalize_receiver_name(const std::string& input) {
    const auto first = input.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto name = input.substr(first, input.find_last_not_of(' ') - first + 1);
    if (name.size() > 50) return {};
    for (std::size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i++]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f || c == '.' || c == '\\') return {};
            continue;
        }
        int count = 0;
        unsigned value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 0x07; minimum = 0x10000; }
        else return {};
        while (count--) {
            if (i == name.size()) return {};
            const auto next = static_cast<unsigned char>(name[i++]);
            if ((next & 0xc0) != 0x80) return {};
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return {};
    }
    return name;
}

} // namespace ap::airplay
