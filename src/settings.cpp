#include "settings.h"
#include "airplay/live_settings.h"
#include "airplay/receiver_name.h"
#include "log.h"

#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <shlobj.h>
#endif

namespace ap {

std::filesystem::path settings_path() {
#if defined(_WIN32)
    PWSTR wide_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wide_path))) {
        CoTaskMemFree(wide_path);
        return {};
    }
    std::filesystem::path base(wide_path);
    CoTaskMemFree(wide_path);
    return base / L"AirPlay-Windows" / L"settings.ini";
#else
    return {};
#endif
}

PersistentSettings load_settings(const std::filesystem::path& path) {
    PersistentSettings s;
    if (path.empty()) return s;

    std::ifstream in(path);
    if (!in.is_open()) return s;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        try {
            if (key == "receiver_name") {
                const auto name = airplay::normalize_receiver_name(val);
                if (!name.empty()) s.receiver_name = name;
            }
            else if (key == "mirror_width") {
                const int n = std::stoi(val);
                if (n >= 320 && n <= 7680) s.mirror_width = n;
            }
            else if (key == "mirror_height") {
                const int n = std::stoi(val);
                if (n >= 240 && n <= 4320) s.mirror_height = n;
            }
            else if (key == "hevc_enabled")   s.hevc_enabled   = std::stoi(val) != 0;
            else if (key == "max_fps") {
                const int n = std::stoi(val);
                if (n >= 1 && n <= 60) s.max_fps = n;
            }
            else if (key == "refresh_rate") {
                const int n = std::stoi(val);
                if (n >= 1 && n <= 60) s.refresh_rate = n;
            }
            else if (key == "mirror_hwaccel") s.mirror_hwaccel = std::stoi(val) != 0;
            else if (key == "vsync_enabled")  s.vsync_enabled  = std::stoi(val) != 0;
            else if (key == "fullscreen")     s.fullscreen     = std::stoi(val) != 0;
            else if (key == "chromeless")     s.chromeless     = std::stoi(val) != 0;
            else if (key == "video_rotation") {
                const int n = std::stoi(val);
                if (n >= -1 && n <= 3) s.video_rotation = n;
            }
            else if (key == "diagnostics") s.ui.diagnostics = std::stoi(val) != 0;
            else if (key == "window_placement_valid") s.ui.window_placement_valid = std::stoi(val) != 0;
            else if (key == "window_maximized") s.ui.window_maximized = std::stoi(val) != 0;
            else if (key == "window_x" || key == "window_y") {
                const int n = std::stoi(val);
                if (n >= -100000 && n <= 100000) {
                    if (key == "window_x") s.ui.window_x = n;
                    else s.ui.window_y = n;
                }
            }
            else if (key == "window_width" || key == "window_height") {
                const int n = std::stoi(val);
                if (n >= 160 && n <= 32768) {
                    if (key == "window_width") s.ui.window_width = n;
                    else s.ui.window_height = n;
                }
            }
            else if (key == "recording_directory") s.ui.recording_directory = val;
            else if (key == "recording_codec") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 1) s.ui.recording_codec = n;
            }
            else if (key == "recording_encoder") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 2) s.ui.recording_encoder = n;
            }
            else if (key == "recording_bitrate") {
                const int n = std::stoi(val);
                if (n >= 2 && n <= 40) s.ui.recording_bitrate = n;
            }
        } catch (...) {
            LOG_WARN << "settings: malformed value for " << key << "=" << val;
        }
    }
    LOG_INFO << "settings: loaded from " << path.u8string();
    return s;
}

bool save_settings(const PersistentSettings& s, const std::filesystem::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_WARN << "settings: could not create directory: " << ec.message();
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN << "settings: could not write " << path.u8string();
        return false;
    }

    out << "receiver_name=" << s.receiver_name << '\n'
        << "mirror_width="   << s.mirror_width   << '\n'
        << "mirror_height="  << s.mirror_height  << '\n'
        << "hevc_enabled="   << s.hevc_enabled   << '\n'
        << "max_fps="        << s.max_fps        << '\n'
        << "refresh_rate="   << s.refresh_rate   << '\n'
        << "mirror_hwaccel=" << s.mirror_hwaccel << '\n'
        << "vsync_enabled="  << s.vsync_enabled  << '\n'
        << "fullscreen="     << s.fullscreen     << '\n'
        << "chromeless="     << s.chromeless     << '\n'
        << "video_rotation=" << s.video_rotation << '\n'
        << "diagnostics=" << s.ui.diagnostics << '\n'
        << "window_placement_valid=" << s.ui.window_placement_valid << '\n'
        << "window_x=" << s.ui.window_x << '\n'
        << "window_y=" << s.ui.window_y << '\n'
        << "window_width=" << s.ui.window_width << '\n'
        << "window_height=" << s.ui.window_height << '\n'
        << "window_maximized=" << s.ui.window_maximized << '\n'
        << "recording_directory=" << s.ui.recording_directory << '\n'
        << "recording_codec=" << s.ui.recording_codec << '\n'
        << "recording_encoder=" << s.ui.recording_encoder << '\n'
        << "recording_bitrate=" << s.ui.recording_bitrate << '\n';
    out.close();
    if (!out) {
        LOG_WARN << "settings: could not finish writing " << temporary.u8string();
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_WARN << "settings: could not replace file, Windows error " << GetLastError();
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) return false;
#endif

    LOG_INFO << "settings: saved to " << path.u8string();
    return true;
}

PersistentSettings snapshot(const ap::airplay::LiveSettings& live) {
    PersistentSettings s;
    s.receiver_name = live.receiver_name();
    s.mirror_width   = live.mirror_width.load(std::memory_order_relaxed);
    s.mirror_height  = live.mirror_height.load(std::memory_order_relaxed);
    s.hevc_enabled   = live.hevc_enabled.load(std::memory_order_relaxed);
    s.max_fps        = live.max_fps.load(std::memory_order_relaxed);
    s.refresh_rate   = live.refresh_rate.load(std::memory_order_relaxed);
    s.mirror_hwaccel = live.mirror_hwaccel.load(std::memory_order_relaxed);
    s.vsync_enabled  = live.vsync_enabled.load(std::memory_order_relaxed);
    s.fullscreen     = live.fullscreen.load(std::memory_order_relaxed);
    s.chromeless     = live.chromeless.load(std::memory_order_relaxed);
    s.video_rotation = live.video_rotation.load(std::memory_order_relaxed);
    s.ui = live.ui_preferences();
    return s;
}

} // namespace ap
