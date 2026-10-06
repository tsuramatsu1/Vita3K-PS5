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
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace platform {

enum class PlatformKind {
    Desktop,
    PS5,
};

// Where a runtime without a desktop UI keeps its files
struct RuntimeConfig {
    PlatformKind kind = PlatformKind::Desktop;
    std::string app_name;
    // Read-only files shipped with the emulator (data, shaders-builtin)
    std::string static_assets_directory;
    // Writable root of the emulated file system, config, logs and caches
    std::string save_directory;
};

class PlatformInterface {
public:
    virtual ~PlatformInterface() = default;
    virtual RuntimeConfig config() const = 0;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
};

const char *platform_name(PlatformKind kind);
// Shows a short message over whatever is on screen: the PS5's system toast; nothing on desktop
void notify(const std::string &message);

// Tells the console that someone is still here. It drops to an idle power state a minute after the last button
// press, which costs a long install most of its speed; call this while working through something lengthy
void keep_awake();

// Reports why a path could not be removed: what it is, who owns it, the flags set on it, and what happens when its
// first entry is unlinked. Removal refused on a file this process owns, in a folder it can write, usually means a
// flag was set on the file rather than a permission being missing
void report_undeletable(const std::string &path);

// Removes a folder and everything in it. Returns the number of entries that would not go, so zero means it is gone
int remove_tree(const std::string &path);
// Ends the PS5 shell's launch splash (sce_sys/pic1.dds), once there is a frame to show; nothing on desktop
void hide_splash_screen();
std::unique_ptr<PlatformInterface> create_platform();

} // namespace platform
