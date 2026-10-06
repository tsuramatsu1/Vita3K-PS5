// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// PS5 entry point: the Android frontend's boot flow without a UI. The console starts a title with no
// command line, so the app to run is named by a file in the save directory

#include "archive.h"
#include "frontend/frontend.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <compat/functions.h>
#include <config/functions.h>
#include <config/state.h>
#include <config/version.h>
#include <ctrl/functions.h>
#include <emuenv/state.h>
#include <io/device.h>
#include <modules/module_parent.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <overlay/display_manager.h>
#include <overlay/pointer.h>
#include <platform/pad.h>
#include <platform/platform.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <audio/state.h>
#include <touch/functions.h>
#include <touch/state.h>
#include <util/exit_code.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <SDL3/SDL.h>

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *arguments);
extern "C" int sceKernelUsleep(unsigned int microseconds);

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char *BOOT_ARGUMENT = "--boot";
// With --boot, where the app is when it is not installed: its folder on USB storage
constexpr const char *APP_DIRECTORY_ARGUMENT = "--app-directory";
// Set when the title restarts itself, so the splash plays when the player opened it and not on the way back
constexpr const char *NO_SPLASH_ARGUMENT = "--no-splash";
// With --boot, the self inside the app to run instead of its eboot, and one --self-arg per argument to pass it.
// sceAppMgrLoadExec is served by restarting the title with these rather than relaunching inside this process
constexpr const char *SELF_ARGUMENT = "--self";
constexpr const char *SELF_ARG_ARGUMENT = "--self-arg";
constexpr const char *INSTALL_DIRECTORY_NAME = "install";
constexpr int USB_DRIVES = 8;
constexpr int USB_SEARCH_DEPTH = 3;

// The mode of VideoOut, the one display the Vulkan driver reports
constexpr int VIDEOOUT_WIDTH = 3840;
constexpr int VIDEOOUT_HEIGHT = 2160;

class Ps5FrameHost final : public renderer::FrameHost {
public:
    renderer::DisplayHandle handle() const override {
        return renderer::Ps5DisplayHandle{};
    }

    int drawable_width() const override {
        return VIDEOOUT_WIDTH;
    }

    int drawable_height() const override {
        return VIDEOOUT_HEIGHT;
    }

    std::vector<std::string> font_dirs() const override {
        return {};
    }
};

bool create_directories(const Root &root_paths) {
    try {
        fs::create_directories(root_paths.get_vita_fs_path());
        fs::create_directories(root_paths.get_config_path());
        fs::create_directories(root_paths.get_cache_path());
        fs::create_directories(root_paths.get_log_path() / "shaderlog");
        fs::create_directories(root_paths.get_log_path() / "texturelog");
        fs::create_directories(root_paths.get_patch_path());
        fs::create_directories(root_paths.get_shared_path() / "textures");
        return true;
    } catch (const std::exception &e) {
        // Logging needs these directories, so this goes to standard error, which reaches klog
        std::fprintf(stderr, "cannot create the Vita3K directories: %s\n", e.what());
        return false;
    }
}

bool has_extension(const fs::path &path, std::initializer_list<const char *> extensions) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::find_if(extensions.begin(), extensions.end(), [&](const char *candidate) { return extension == candidate; }) != extensions.end();
}

// What the installer takes: the firmware, an app, a package, or the licence that unlocks a package. A licence is
// named work.bin by the tool that makes it, and .rif by the console
bool is_installable(const fs::path &path) {
    return has_extension(path, { ".pup", ".vpk", ".zip", ".pkg", ".rif" })
        || string_utils::tolower(path.filename().string()) == "work.bin";
}

// What the installer's browser shows for a folder. With no path, the places worth starting from: the install folder
// and whichever USB drives are plugged in
std::vector<ps5::frontend::Entry> list_directory(const fs::path &install_directory, const std::string &path) {
    std::vector<ps5::frontend::Entry> entries;
    boost::system::error_code error;

    if (path.empty()) {
        if (fs::is_directory(install_directory, error))
            entries.push_back({ "install", install_directory.string(), true });
        // The storage Vita3K was given, which is as wide as a title can reach outside its own files: /data once it
        // is out of its sandbox, and its own download folder while it is not
        const fs::path storage = install_directory.parent_path().parent_path();
        if (fs::is_directory(storage, error))
            entries.push_back({ storage.filename().string(), storage.string(), true });
        // The USB drives, which a title only sees once it is out of its sandbox
        for (int drive = 0; drive < USB_DRIVES; drive++) {
            const fs::path mount = fmt::format("/mnt/usb{}", drive);
            if (fs::is_directory(mount, error))
                entries.push_back({ mount.filename().string(), mount.string(), true });
        }
        return entries;
    }

    fs::directory_iterator entry(fs::path(path), error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        const fs::path &found = entry->path();
        const std::string name = found.filename().string();
        // The console's own bookkeeping, which is not ours to browse
        if (name.empty() || name.front() == '.' || name.front() == '$')
            continue;
        if (fs::is_directory(found, error)) {
            entries.push_back({ name, found.string(), true });
        } else if (fs::is_regular_file(found, error)) {
            entries.push_back({ name, found.string(), false, is_installable(found),
                fs::file_size(found, error) });
        }
    }

    // Folders first, then files, each by name
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.directory == b.directory ? a.name < b.name : a.directory;
    });
    return entries;
}

// A folder the player picked to look in for games, remembered between runs beside the rest of the configuration
fs::path games_folder_record(const fs::path &save_directory) {
    return save_directory / "config" / "games-folder.txt";
}

std::string read_games_folder(const fs::path &save_directory) {
    std::ifstream record(games_folder_record(save_directory).string());
    std::string path;
    std::getline(record, path);
    return path;
}

void write_games_folder(const fs::path &save_directory, const std::string &path) {
    boost::system::error_code error;
    fs::create_directories(games_folder_record(save_directory).parent_path(), error);
    std::ofstream record(games_folder_record(save_directory).string(), std::ios::trunc);
    record << path << "\n";
    LOG_INFO("Games folder set to {}", path);
}

// The settings the front end offers, each a short list of choices rather than a number to type. Changing one writes
// the configuration straight back out, so it survives the restart that starting a game performs
constexpr const char *ANALOG_SETTING = "Stick sensitivity";
constexpr const char *TOUCH_SETTING = "Touchpad acts as";
constexpr const char *VOLUME_SETTING = "Volume";
constexpr const char *LOG_SETTING = "Logging";
constexpr const char *ACCURACY_SETTING = "Rendering";
constexpr const char *SURFACE_SYNC_SETTING = "Surface write-back";
constexpr const char *MAPPING_SETTING = "Memory mapping";
constexpr const char *ASYNC_SETTING = "Shader compilation";

const std::vector<float> ANALOG_CHOICES = { 1.0f, 1.1f, 1.25f, 1.4f, 1.6f, 2.0f };
const std::vector<int> VOLUME_CHOICES = { 0, 25, 50, 75, 100 };
const std::vector<std::string> LOG_CHOICES = { "trace", "debug", "info", "warning", "error" };
const std::vector<std::string> MAPPING_CHOICES = { "double-buffer", "page-table", "external-host", "disabled" };
const std::vector<std::string> MAPPING_LABELS = { "double buffer", "page table", "external host", "off" };

// The choice nearest what the configuration already holds, so an unusual value does not jump when it is shown
template <typename T>
int nearest_choice(const std::vector<T> &choices, T value) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(choices.size()); i++) {
        if (std::abs(static_cast<double>(choices[i] - value)) < std::abs(static_cast<double>(choices[best] - value)))
            best = i;
    }
    return best;
}

std::vector<ps5::frontend::Setting> read_settings(EmuEnvState &emuenv) {
    std::vector<ps5::frontend::Setting> settings;

    std::vector<std::string> analog;
    for (float value : ANALOG_CHOICES)
        analog.push_back(fmt::format("{:.2g}x", value));
    settings.push_back({ ANALOG_SETTING,
        "How far a game sees the sticks pushed. Raise it when a character walks where it should run, because a "
        "stick that does not quite reach its own extremes leaves a game short of full tilt.",
        analog, nearest_choice(ANALOG_CHOICES, emuenv.cfg.controller_analog_multiplier) });

    settings.push_back({ TOUCH_SETTING,
        "Which of the Vita's two touch panels the controller's touchpad stands in for. Most games use the front; "
        "a few ask for the rear panel instead.",
        { "the front screen", "the rear panel" }, emuenv.touch.touchscreen_port == SCE_TOUCH_PORT_BACK ? 1 : 0 });

    std::vector<std::string> volumes;
    for (int value : VOLUME_CHOICES)
        volumes.push_back(fmt::format("{}%", value));
    settings.push_back({ VOLUME_SETTING, "How loud games play.", volumes,
        nearest_choice(VOLUME_CHOICES, emuenv.cfg.audio_volume) });

    // The configuration holds spdlog's own numbering, which these choices are listed in
    settings.push_back({ LOG_SETTING,
        "How much detail goes to the log. Tracing records every file a game touches, which is worth having when "
        "something is wrong and a needless cost when it is not.",
        LOG_CHOICES, std::clamp(emuenv.cfg.log_level, 0, static_cast<int>(LOG_CHOICES.size()) - 1) });

    settings.push_back({ ACCURACY_SETTING,
        "Accurate drawing gives up two shortcuts the renderer otherwise takes with render targets. Try it on a game "
        "whose picture breaks up into blocks or stripes; it costs frames, so leave it off when the picture is fine.",
        { "fast", "accurate" }, emuenv.cfg.high_accuracy ? 1 : 0 });

    settings.push_back({ SURFACE_SYNC_SETTING,
        "Whether what the graphics chip draws is copied back into the game's own memory. A game that reads its "
        "render targets back, or builds a picture over several passes, shows black or stale rectangles without it.",
        { "off", "on" }, emuenv.cfg.disable_surface_sync ? 0 : 1 });

    const auto mapping = std::find(MAPPING_CHOICES.begin(), MAPPING_CHOICES.end(), emuenv.cfg.memory_mapping);
    settings.push_back({ MAPPING_SETTING,
        "How a game's own memory reaches the graphics chip. Turning it off is the slowest and the surest; the other "
        "three differ in how they keep the two copies in step, and a game that draws stale or missing geometry is "
        "worth trying on each.",
        MAPPING_LABELS, mapping == MAPPING_CHOICES.end() ? 0 : static_cast<int>(mapping - MAPPING_CHOICES.begin()) });

    settings.push_back({ ASYNC_SETTING,
        "Compiling shaders in the background keeps a game moving, at the cost of leaving whatever is not compiled "
        "yet undrawn for a moment. Waiting for each one stutters instead, but nothing is ever missing.",
        { "in the background", "wait for each" }, emuenv.cfg.async_pipeline_compilation ? 0 : 1 });

    return settings;
}

void write_setting(EmuEnvState &emuenv, const Root &root_paths, const ps5::frontend::Setting &setting) {
    if (setting.name == ANALOG_SETTING) {
        emuenv.cfg.controller_analog_multiplier = ANALOG_CHOICES[setting.choice];
    } else if (setting.name == TOUCH_SETTING) {
        emuenv.touch.touchscreen_port = setting.choice == 1 ? SCE_TOUCH_PORT_BACK : SCE_TOUCH_PORT_FRONT;
    } else if (setting.name == VOLUME_SETTING) {
        emuenv.cfg.audio_volume = VOLUME_CHOICES[setting.choice];
        emuenv.cfg.current_config.audio_volume = emuenv.cfg.audio_volume;
        emuenv.audio.set_global_volume(emuenv.cfg.audio_volume / 100.f);
    } else if (setting.name == LOG_SETTING) {
        emuenv.cfg.log_level = static_cast<spdlog::level::level_enum>(setting.choice);
        logging::set_level(static_cast<spdlog::level::level_enum>(setting.choice));
    } else if (setting.name == ACCURACY_SETTING) {
        emuenv.cfg.high_accuracy = setting.choice == 1;
    } else if (setting.name == SURFACE_SYNC_SETTING) {
        emuenv.cfg.disable_surface_sync = setting.choice == 0;
    } else if (setting.name == MAPPING_SETTING) {
        emuenv.cfg.memory_mapping = MAPPING_CHOICES[setting.choice];
    } else if (setting.name == ASYNC_SETTING) {
        emuenv.cfg.async_pipeline_compilation = setting.choice == 0;
    }

    if (config::serialize_config(emuenv.cfg, root_paths.get_config_path()) != Success)
        LOG_ERROR("Could not write the configuration back");
}

// Removes an installed game: its own folder, wherever it was installed or found, and the record of it in the list
bool remove_game(EmuEnvState &emuenv, const std::string &title_id) {
    const fs::path directory = device::app_directory(emuenv.vita_fs_path, title_id);
    const int left = platform::remove_tree(fs_utils::path_to_utf8(directory));
    if (left > 0)
        LOG_ERROR("{} entries of {} would not go", left, directory);
    else
        LOG_INFO("Removed {} from {}", title_id, directory);
    app::init_apps_list(emuenv);
    return left == 0;
}

// Where a game can be installed: the console's own storage, and the USB drives that are plugged in. A game on a
// drive lives under <drive>/ux0, which is where the drives are searched for games anyway
std::vector<ps5::frontend::Destination> list_destinations(const fs::path &vita_fs_path) {
    std::vector<ps5::frontend::Destination> destinations{ { "Console storage", vita_fs_path.string() } };
    boost::system::error_code error;
    for (int drive = 0; drive < USB_DRIVES; drive++) {
        const fs::path mount = fmt::format("/mnt/usb{}", drive);
        if (fs::is_directory(mount, error))
            destinations.push_back({ fmt::format("USB drive {}", drive), (mount / "Vita3K").string() });
    }
    return destinations;
}

// Installs the package the player picked. One in the install folder moves to done/ or failed/ once it is installed,
// so it is not offered again. One that did not install is left exactly where it was, to try again or to take away
void install_package(EmuEnvState &emuenv, const fs::path &install_directory, const ps5::frontend::Entry &file, const ps5::frontend::Destination &destination, ps5::frontend::InstallProgress &progress) {
    const fs::path path = fs_utils::utf8_to_path(file.path);
    progress.begin(file.name, 0, 1);
    LOG_INFO("Installing {}", path);

    bool installed = false;
    if (has_extension(path, { ".pup" })) {
        try {
            const std::string version = install_pup(emuenv.vita_fs_path, path, [&](uint32_t percent) {
                LOG_INFO("Firmware installation: {}%", percent);
                progress.progress(percent / 100.0f);
            });
            LOG_INFO("Installed firmware {} from {}", version, file.name);
            progress.finished(fmt::format("Installed firmware {}", version), true);
            installed = true;
        } catch (const std::exception &e) {
            LOG_ERROR("Firmware installation from {} failed: {}", path, e.what());
            progress.finished(fmt::format("Could not install {}", file.name), false);
        }
    } else if (has_extension(path, { ".rif" }) || string_utils::tolower(file.name) == "work.bin") {
        // The licence that unlocks a package. copy_license reads which title it belongs to out of the file itself
        installed = copy_license(emuenv, path);
        LOG_INFO("{} the licence from {}", installed ? "Installed" : "Could not install", path);
        progress.finished(installed ? fmt::format("Installed the licence for {}", emuenv.license_title_id)
                                    : fmt::format("{} is not a licence file", file.name),
            installed);
    } else if (has_extension(path, { ".pkg" })) {
        // A package carries its content encrypted; the licence that unlocks it has to be installed already
        std::string zrif = find_pkg_zrif(path, emuenv.vita_fs_path);
        if (zrif.empty()) {
            LOG_ERROR("No licence for {}: install its .rif first, or use the .vpk", path);
            progress.finished(fmt::format("{} needs its licence (.rif) installed first", file.name), false);
        } else {
            installed = install_pkg(
                path, emuenv, zrif, [&](float percent) { progress.progress(percent / 100.0f); },
                [&](const std::string &item, float of_file) { progress.detail(item, of_file); },
                [&] { return progress.cancelled(); });
            const bool stopped = !installed && progress.cancelled();
            LOG_INFO("{} {} from {}", installed ? "Installed" : stopped ? "Cancelled" : "Could not install",
                emuenv.app_info.app_title_id, file.name);
            progress.finished(stopped ? fmt::format("Cancelled; {} was not installed", file.name)
                                      : fmt::format("{} {} ({})", installed ? "Installed" : "Could not install",
                                            emuenv.app_info.app_title, emuenv.app_info.app_title_id),
                installed);
        }
    } else {
        const fs::path into = destination.path.empty() ? emuenv.vita_fs_path : fs_utils::utf8_to_path(destination.path);
        const std::vector<ContentInfo> contents = install_archive(
            emuenv, path,
            [&](const ArchiveContents &state) {
                if (state.progress)
                    progress.progress(*state.progress / 100.0f);
                if (state.item)
                    progress.detail(*state.item, state.item_progress.value_or(0.0f));
            },
            nullptr, into);
        installed = !contents.empty() && std::all_of(contents.begin(), contents.end(), [](const ContentInfo &content) { return content.state; });
        for (const ContentInfo &content : contents) {
            LOG_INFO("{} {} ({}) from {}", content.state ? "Installed" : "Could not install", content.title, content.title_id, file.name);
            progress.finished(fmt::format("{} {} ({})", content.state ? "Installed" : "Could not install", content.title, content.title_id), content.state);
        }
        if (contents.empty())
            progress.finished(fmt::format("Could not install {}: see the log", file.name), false);
    }

    if (installed) {
        // The package has served its purpose and is the size of the game itself; a failed one stays for another try
        if (platform::remove_tree(fs_utils::path_to_utf8(path)) == 0)
            LOG_INFO("Removed {}", path);
        else
            LOG_WARN("Could not remove {}", path);
    }
    app::init_apps_list(emuenv);
}

bool initialize_emulator(EmuEnvState &emuenv, const Root &root_paths) {
    Config cfg{};
    char arg0[] = "Vita3K";
    char *argv[] = { arg0, nullptr };
    if (config::init_config(cfg, 1, argv, root_paths, false) != Success) {
        LOG_ERROR("Failed to initialise config");
        return false;
    }

    fs::create_directories(cfg.get_vita_fs_path());

    if (!app::init(emuenv, cfg, root_paths)) {
        LOG_ERROR("Failed to initialise emulated environment");
        return false;
    }

    emuenv.vulkan_device_info = std::make_unique<renderer::VulkanDeviceInfo>(renderer::enumerate_vulkan_devices());

    if (emuenv.cfg.controller_binds.size() != 15 || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);

    if (!app::init_apps_list(emuenv))
        LOG_ERROR("Failed to initialise apps list");

    app::load_users(emuenv);
    if (!app::ensure_current_user(emuenv)) {
        LOG_ERROR("Failed to initialise active user");
        return false;
    }

    compat::load_from_disk(emuenv.compat, std::filesystem::path(emuenv.cache_path.string()));
    return true;
}

bool start_session(EmuEnvState &emuenv, app::AppSessionController &session, renderer::FrameHost &frame_host, const AppLaunchRequest &launch_request) {
    if (!session.begin_launch(launch_request, launch_request.reason != AppLaunchReason::LoadExec)) {
        LOG_ERROR("{} is not installed in {}", launch_request.app_path, emuenv.vita_fs_path);
        return false;
    }
    if (!session.initialize_renderer(frame_host)) {
        LOG_ERROR("Failed to initialise renderer");
        return false;
    }
    if (!session.initialize_runtime()) {
        LOG_ERROR("Failed late initialisation");
        return false;
    }
    if (!session.load_and_run()) {
        LOG_ERROR("Failed to load or start the app session");
        return false;
    }
    return true;
}

// The DualSense's touchpad standing in for the Vita's front panel. SDL has no driver for the console's controller,
// so the events the emulator's touch handling already understands are made here from what scePad reports. The pad
// gives a finger's position and its own id each frame; a press, a move and a release are the differences between
// one frame and the next
void feed_touchpad(EmuEnvState &emuenv, const platform::PadSample &pad) {
    // The Vita's panel is its screen: you touch what you see. The controller's touchpad is a small blind surface,
    // so dragging a finger across it moves a pointer instead, and clicking the pad taps wherever that pointer is
    constexpr float SCREEN_WIDTH = 960.0f;
    constexpr float SCREEN_HEIGHT = 544.0f;
    constexpr float SPEED = 1.6f; // how far the pointer travels for a given drag

    static float cursor_x = SCREEN_WIDTH / 2.0f;
    static float cursor_y = SCREEN_HEIGHT / 2.0f;
    static bool was_touching = false;
    static float last_x = 0.0f, last_y = 0.0f;
    static bool was_clicked = false;

    const bool touching = pad.touch_count > 0;
    if (touching) {
        if (was_touching) {
            cursor_x = std::clamp(cursor_x + (pad.touches[0].x - last_x) * SCREEN_WIDTH * SPEED, 0.0f, SCREEN_WIDTH);
            cursor_y = std::clamp(cursor_y + (pad.touches[0].y - last_y) * SCREEN_HEIGHT * SPEED, 0.0f, SCREEN_HEIGHT);
        }
        last_x = pad.touches[0].x;
        last_y = pad.touches[0].y;
    }
    was_touching = touching;

    // Clicking the touchpad is the tap, so the pointer can be placed before anything is pressed
    const bool clicked = (pad.buttons & platform::pad_buttons::TOUCHPAD) != 0;

    auto &mouse = emuenv.ctrl.overlay_mouse;
    mouse.x.store(cursor_x, std::memory_order_relaxed);
    mouse.y.store(cursor_y, std::memory_order_relaxed);
    mouse.pressed.store(clicked, std::memory_order_relaxed);

    // Drawn over the game, since a touchpad gives no sign of where it is aiming. It appears once the pad is used
    // and stays, so the player can place it before committing to a tap
    if (emuenv.overlay_manager) {
        auto marker = emuenv.overlay_manager->get<overlay::pointer>();
        if (!marker && (touching || clicked))
            marker = emuenv.overlay_manager->create<overlay::pointer>();
        if (marker) {
            marker->set_position(cursor_x, cursor_y, clicked);
            marker->visible.store(true, std::memory_order_relaxed);
        }
    }

    // The same pointer reaches a game that reads the touch panel itself
    const auto send = [&](SDL_EventType type) {
        SDL_GamepadTouchpadEvent event{};
        event.type = type;
        event.touchpad = 0;
        event.finger = 0;
        event.x = cursor_x / SCREEN_WIDTH;
        event.y = cursor_y / SCREEN_HEIGHT;
        event.pressure = type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? 0.0f : 1.0f;
        handle_touchpad_event(emuenv.touch, event);
    };

    if (clicked && !was_clicked) {
        LOG_INFO("Pointer tap at {:.0f},{:.0f}", cursor_x, cursor_y);
        send(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN);
    } else if (!clicked && was_clicked) {
        send(SDL_EVENT_GAMEPAD_TOUCHPAD_UP);
    } else if (clicked) {
        send(SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION);
    }
    was_clicked = clicked;
}

// Both sticks pressed in, held: the Vita has no stick buttons at all, so no game can want this combination. The
// hold is what keeps a knock against a stick from ending the game
bool exit_shortcut_held() {
    constexpr std::uint32_t COMBINATION = platform::pad_buttons::L3 | platform::pad_buttons::R3;
    constexpr std::uint64_t HOLD_US = 700000;
    static std::uint64_t held_since = 0;

    // Read here rather than wait on the game's own polling, so this still works when a game has stopped reading
    const platform::PadSample pad = platform::poll_pad();
    const std::uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
    if ((pad.buttons & COMBINATION) != COMBINATION) {
        held_since = 0;
        return false;
    }
    if (held_since == 0)
        held_since = now;
    return now - held_since >= HOLD_US;
}

// Starts the title again with no game to boot, which is the game list
[[noreturn]] void return_to_game_list() {
    LOG_INFO("Restarting into the game list");
    logging::flush();
    platform::notify("Vita3K: returning to the game list");
    // An empty list, not a null one: the shell refuses the call without somewhere to read the arguments from
    const char *const arguments[] = { NO_SPLASH_ARGUMENT, nullptr };
    const int refused = sceSystemServiceLoadExec("/app0/eboot.bin", arguments);
    LOG_ERROR("The system refused to restart the title: 0x{:X}", static_cast<unsigned>(refused));
    // LoadExec does not return on success; park rather than run on with a stopped session
    for (;;)
        sceKernelUsleep(100000);
}

// The front end has nothing to draw while a game runs, so it idles at roughly a frame between polls
constexpr int FRAME_INTERVAL_MS = 16;

// Serves sceAppMgrLoadExec by restarting the title to run the next self, which is how picking a game from the
// front end already works. Relaunching inside this process means stopping the session while the game's own threads
// are still live, and that faults on host state they are holding
[[noreturn]] void restart_for_launch(const AppLaunchRequest &launch_request) {
    LOG_INFO("Restarting to run {} of {}", launch_request.self_path, launch_request.app_path);
    logging::flush();

    std::vector<std::string> owned{ BOOT_ARGUMENT, launch_request.app_path, NO_SPLASH_ARGUMENT };
    const auto external = device::external_apps();
    if (const auto found = external.find(launch_request.app_path); found != external.end()) {
        owned.emplace_back(APP_DIRECTORY_ARGUMENT);
        owned.push_back(found->second.string());
    }
    if (!launch_request.self_path.empty()) {
        owned.emplace_back(SELF_ARGUMENT);
        owned.push_back(launch_request.self_path);
    }
    for (const std::string &argument : launch_request.argv) {
        owned.emplace_back(SELF_ARG_ARGUMENT);
        owned.push_back(argument);
    }

    std::vector<const char *> arguments;
    arguments.reserve(owned.size() + 1);
    for (const std::string &argument : owned)
        arguments.push_back(argument.c_str());
    arguments.push_back(nullptr);

    const int refused = sceSystemServiceLoadExec("/app0/eboot.bin", arguments.data());
    LOG_ERROR("The system refused to restart the title: 0x{:X}", static_cast<unsigned>(refused));
    // LoadExec does not return on success; park rather than run on with a session that is about to be torn down
    for (;;)
        sceKernelUsleep(100000);
}

int run_app(EmuEnvState &emuenv, const std::string &title_id, const std::string &self_path, const std::vector<std::string> &self_argv) {
    app::AppSessionController session(emuenv);
    Ps5FrameHost frame_host;
    AppLaunchRequest launch_request{ .app_path = title_id, .self_path = self_path, .argv = self_argv,
        .reason = self_path.empty() ? AppLaunchReason::User : AppLaunchReason::LoadExec };

    while (true) {
        LOG_INFO("Booting {}", launch_request.app_path);
        platform::notify(fmt::format("Vita3K: starting {}", launch_request.app_path));
        if (!start_session(emuenv, session, frame_host, launch_request)) {
            // Leaving to the console's own home screen tells the player nothing. Going back to the list, with a
            // word about what happened, at least says which game it was and leaves them somewhere useful
            LOG_ERROR("{} would not start", launch_request.app_path);
            logging::flush();
            platform::notify(fmt::format("Vita3K: {} would not start; see the log", launch_request.app_path));
            session.stop(app::AppSessionStopReason::LaunchFailure);
            return_to_game_list();
        }

        LOG_INFO("Game started: {} ({})", emuenv.current_app_title, launch_request.app_path);
        platform::hide_splash_screen();
        app::LaunchRuntimeMetrics runtime_metrics{};
        std::optional<AppLaunchRequest> next_launch_request = emuenv.take_app_launch_request();

        while (!next_launch_request && session.is_running()) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
                    refresh_controllers(emuenv.ctrl, emuenv);
            }

            if (exit_shortcut_held()) {
                LOG_INFO("Leaving {} for the game list", launch_request.app_path);
                // The session is left standing: the shell kills this process to start the next one, and tearing the
                // session down first faults on the guest memory the game's own threads are still running in
                return_to_game_list();
            }

            next_launch_request = emuenv.take_app_launch_request();
            app::update_runtime_metrics(emuenv, runtime_metrics);
            platform::keep_awake();
            feed_touchpad(emuenv, platform::current_pad());

            // request_process_exit only posts the request: the guest keeps running until the session is torn down,
            // and a game that has just called sceAppMgrLoadExec spends that window in its own shutdown path. Picking
            // the request up once a frame leaves it up to a frame to fault on state the relaunch is about to take
            // away, so idle in short steps and watch for the request rather than sleeping through the whole frame
            for (int slept = 0; slept < FRAME_INTERVAL_MS && !next_launch_request && session.is_running(); slept++) {
                SDL_Delay(1);
                next_launch_request = emuenv.take_app_launch_request();
            }
        }

        // A ProcessExit request is only a notification that the guest ended and carries no app to boot. Returning
        // from here would end the title itself and drop the player on the console's home screen rather than the list
        if (!next_launch_request || next_launch_request->reason == AppLaunchReason::ProcessExit) {
            LOG_INFO("{} exited", launch_request.app_path);
            session.stop(app::AppSessionStopReason::UserRequest);
            return_to_game_list();
        }

        restart_for_launch(*next_launch_request);
    }
}

// The title ID after --boot: the front end restarts the title with it to run that game in a fresh process
std::string argument_value(int argc, char *argv[], std::string_view name) {
    for (int i = 0; i + 1 < argc; i++) {
        if (std::string_view(argv[i]) == name)
            return argv[i + 1];
    }
    return {};
}

// Every occurrence, in order: a self can be given more than one argument
std::vector<std::string> argument_values(int argc, char *argv[], std::string_view name) {
    std::vector<std::string> values;
    for (int i = 0; i + 1 < argc; i++) {
        if (std::string_view(argv[i]) == name)
            values.emplace_back(argv[i + 1]);
    }
    return values;
}

// A Vita app's folder has a param.sfo and an eboot.bin that is a Vita SELF ("SCE\0"); a PS4 app's has both too,
// but its eboot.bin starts with 4F 15 3D 1D
// A Vita executable begins with "SCE\0". A dump still sealed by its licence has ciphertext there instead, as its
// icon and the rest of its files do
bool is_vita_self(const fs::path &path) {
    std::ifstream self(path.string(), std::ios::binary);
    char magic[4] = {};
    return self.read(magic, sizeof(magic)) && std::memcmp(magic, "SCE\0", sizeof(magic)) == 0;
}

bool is_vita_app(const fs::path &directory) {
    boost::system::error_code error;
    return fs::is_regular_file(directory / "sce_sys/param.sfo", error) && is_vita_self(directory / "eboot.bin");
}

// An app whose files are still encrypted: copied in from a dump rather than installed from its package, so nothing
// reads it - not its icon, and not the executable. Only installing it with its licence decrypts it
bool is_sealed_app(const fs::path &directory) {
    boost::system::error_code error;
    return fs::is_regular_file(directory / "sce_sys/param.sfo", error) && !is_vita_self(directory / "eboot.bin");
}

void find_apps_in(EmuEnvState &emuenv, const fs::path &directory, int depth) {
    boost::system::error_code error;
    fs::directory_iterator entry(directory, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        const fs::path &path = entry->path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.' || name.front() == '$' || !fs::is_directory(path, error))
            continue;
        if (!is_vita_app(path)) {
            if (depth > 1)
                find_apps_in(emuenv, path, depth - 1);
            continue;
        }
        vfs::FileBuffer param;
        if (!fs_utils::read_data(path / "sce_sys/param.sfo", param))
            continue;
        sfo::SfoAppInfo info;
        sfo::get_param_info(info, param, emuenv.cfg.sys_lang);
        if (info.app_title_id.empty())
            continue;
        LOG_INFO("Found {} on USB storage at {}", info.app_title_id, path);
        device::set_external_app(emuenv.vita_fs_path, info.app_title_id, path);
    }
}

// Games kept on USB drives run from there, without installing: an app's folder (with its sce_sys and eboot.bin)
// anywhere in the first USB_SEARCH_DEPTH levels of /mnt/usb0 to /mnt/usb7
void find_usb_apps(EmuEnvState &emuenv) {
    for (int drive = 0; drive < USB_DRIVES; drive++) {
        const fs::path mount = fmt::format("/mnt/usb{}", drive);
        // Where the installer puts a game on this drive, which is deeper than a folder copied across by hand
        find_apps_in(emuenv, mount / "Vita3K/ux0/app", 1);
        find_apps_in(emuenv, mount, USB_SEARCH_DEPTH);
    }
}

// The front end: installs what was dropped, then lets the player pick a game. Picking one restarts the title to run
// it, so the emulator gets a process, and a display, of its own
int run_front_end(EmuEnvState &emuenv, const Root &root_paths, const fs::path &save_directory, bool show_splash) {
    const fs::path install_directory = save_directory / INSTALL_DIRECTORY_NAME;
    const std::string title_id = ps5::frontend::run(
        [&](const ps5::frontend::Entry &file, const ps5::frontend::Destination &destination, ps5::frontend::InstallProgress &progress) {
            install_package(emuenv, install_directory, file, destination, progress);
        },
        [&](const std::string &path) { return list_directory(install_directory, path); },
        [&] { return list_destinations(emuenv.vita_fs_path); },
        [&](const std::string &path) { write_games_folder(save_directory, path); },
        [&](const std::string &title_id) { return remove_game(emuenv, title_id); },
        ps5::frontend::SettingsAccess{
            [&] { return read_settings(emuenv); },
            [&](const ps5::frontend::Setting &setting) { write_setting(emuenv, root_paths, setting); },
        },
        show_splash,
        [&] {
            // Apps copied in by hand since the list was cached are found too, those on USB drives, and those in
            // whichever folder the player pointed the front end at
            find_usb_apps(emuenv);
            if (const std::string folder = read_games_folder(save_directory); !folder.empty())
                find_apps_in(emuenv, fs_utils::utf8_to_path(folder), USB_SEARCH_DEPTH);
            app::scan_apps(emuenv);
            std::vector<ps5::frontend::Game> games;
            const std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
            for (const app::AppEntry &entry : emuenv.app.apps_list.apps) {
                if (!entry.category.starts_with("gd"))
                    continue;
                const fs::path directory = device::app_directory(emuenv.vita_fs_path, entry.title_id);
                games.push_back({ entry.title, entry.title_id, (directory / "sce_sys/icon0.png").string(),
                    is_sealed_app(directory) });
            }
            std::sort(games.begin(), games.end(), [](const auto &a, const auto &b) { return a.title < b.title; });
            return games;
        });
    if (title_id.empty())
        return Success;

    LOG_INFO("Restarting to run {}", title_id);
    const auto external = device::external_apps();
    const auto found = external.find(title_id);
    const std::string app_directory = found != external.end() ? found->second.string() : std::string();
    const char *const arguments[] = { BOOT_ARGUMENT, title_id.c_str(),
        app_directory.empty() ? nullptr : APP_DIRECTORY_ARGUMENT, app_directory.c_str(), nullptr };
    const int refused = sceSystemServiceLoadExec("/app0/eboot.bin", arguments);
    LOG_ERROR("The system refused to restart the title: 0x{:X}", static_cast<unsigned>(refused));
    return InitConfigFailed;
}

} // namespace

int main(int argc, char *argv[]) {
    const std::unique_ptr<platform::PlatformInterface> runtime = platform::create_platform();
    const bool stderr_captured = runtime->initialize();
    const platform::RuntimeConfig runtime_cfg = runtime->config();

    Root root_paths;
    app::init_paths(root_paths, &runtime_cfg);
    if (!create_directories(root_paths))
        return InitConfigFailed;
    if (logging::init(root_paths, true) != Success) {
        std::fprintf(stderr, "cannot start logging in %s\n", root_paths.get_log_path().string().c_str());
        return InitConfigFailed;
    }

    report_crypto_speed();

    LOG_INFO("{} on {}", window_title, platform::platform_name(runtime_cfg.kind));
    if (!stderr_captured)
        LOG_WARN("The log does not reach klog, only {}", root_paths.get_log_path());
    for (int i = 0; i < argc; i++)
        LOG_INFO("Argument {}: {}", i, argv[i]);
    const std::string boot_title_id = argument_value(argc, argv, BOOT_ARGUMENT);
    const bool show_splash = std::none_of(argv, argv + argc, [](const char *argument) {
        return argument && std::string_view(argument) == NO_SPLASH_ARGUMENT;
    });

    // SDL has no PS5 audio driver, and it never picks its silent dummy driver on its own. Until audio goes to
    // sceAudioOut, ask for the dummy so the rest of the emulator starts
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return InitConfigFailed;
    }

    int exit_code = InitConfigFailed;
    {
        auto emuenv = std::make_unique<EmuEnvState>();
        if (initialize_emulator(*emuenv, root_paths)) {
            if (boot_title_id.empty()) {
                exit_code = run_front_end(*emuenv, root_paths, runtime_cfg.save_directory, show_splash);
            } else {
                const std::string app_directory = argument_value(argc, argv, APP_DIRECTORY_ARGUMENT);
                if (!app_directory.empty())
                    device::set_external_app(emuenv->vita_fs_path, boot_title_id, app_directory);
                refresh_controllers(emuenv->ctrl, *emuenv);
                exit_code = run_app(*emuenv, boot_title_id, argument_value(argc, argv, SELF_ARGUMENT),
                    argument_values(argc, argv, SELF_ARG_ARGUMENT));
            }
        }
    }

    SDL_Quit();
    runtime->shutdown();
    return exit_code;
}
