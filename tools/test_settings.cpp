#include "settings.h"
#include "airplay/live_settings.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::current_path() / ("settings-test-" + std::to_string(stamp) + ".ini");
    try {
        ap::PersistentSettings wanted;
        require(wanted.mirror_hwaccel, "GPU acceleration must be the default");
        wanted.receiver_name = "Living room";
        wanted.mirror_width = 3840; wanted.mirror_height = 2160;
        wanted.mirror_hwaccel = true;
        wanted.ui.diagnostics = true;
        wanted.ui.window_placement_valid = true;
        wanted.ui.window_x = -1400; wanted.ui.window_y = 100;
        wanted.ui.window_width = 1100; wanted.ui.window_height = 760;
        wanted.ui.window_maximized = true;
        wanted.ui.recording_directory = "C:/Videos/Clips = \xc3\xa9t\xc3\xa9";
        wanted.ui.recording_codec = 1;
        wanted.ui.recording_encoder = 2;
        wanted.ui.recording_bitrate = 24;
        require(ap::save_settings(wanted, path), "initial save failed");
        require(ap::load_settings(path) == wanted, "preferences lost on reload");
        wanted.ui.diagnostics = false;
        wanted.mirror_hwaccel = false;
        wanted.ui.recording_bitrate = 40;
        require(ap::save_settings(wanted, path), "replacement save failed");
        require(ap::load_settings(path) == wanted, "replacement did not preserve preferences");
        ap::airplay::LiveSettings live;
        live.set_ui_preferences(wanted.ui);
        require(ap::snapshot(live).ui == wanted.ui, "UI preferences missing from live snapshot");
        {
            std::ofstream old(path);
            old << "receiver_name=Legacy receiver\nmirror_width=1920\nmirror_height=1080\nmirror_hwaccel=1\n";
        }
        const auto legacy = ap::load_settings(path);
        require(legacy.receiver_name == "Legacy receiver" && legacy.mirror_hwaccel, "legacy preferences broken");
        require(legacy.ui == ap::UiPreferences{}, "missing fields do not use defaults");
        {
            std::ofstream invalid(path);
            invalid << "window_width=-5\nwindow_height=9999999\nrecording_codec=99\n"
                       "recording_encoder=-1\nrecording_bitrate=0\ndiagnostics=invalid\n";
        }
        require(ap::load_settings(path).ui == ap::UiPreferences{}, "invalid preferences accepted");
        require(!ap::save_settings(wanted, {}), "empty path accepted");
        std::filesystem::remove(path);
        std::cout << "Settings round-trip, replacement, legacy and invalid-value checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
