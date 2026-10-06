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
#include <platform/platform.h>

#include <filesystem>
#include <system_error>

namespace platform {
namespace {

class DesktopPlatform final : public PlatformInterface {
public:
    RuntimeConfig config() const override {
        return RuntimeConfig{
            .kind = PlatformKind::Desktop,
            .app_name = "Vita3K",
        };
    }

    bool initialize() override {
        return true;
    }

    void shutdown() override {
    }
};

} // namespace

void notify(const std::string &) {
}

void keep_awake() {
}

void report_undeletable(const std::string &) {
}

int remove_tree(const std::string &path) {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(path), error);
    return error ? 1 : 0;
}

void hide_splash_screen() {
}

std::unique_ptr<PlatformInterface> create_platform() {
    return std::make_unique<DesktopPlatform>();
}

} // namespace platform
