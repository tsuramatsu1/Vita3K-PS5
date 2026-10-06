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

#include <core/input.hpp>

#include <cstdint>
#include <vector>

namespace ps5::frontend {

// The DualSense of the user who started the title, as the UI kit reads a controller
class Pad {
public:
    bool open();
    // Every sample the console has queued since the last read, oldest first
    const std::vector<hui::PadSample> &read();
    // The buttons down now, so a press still held from before (the Cross that started the title) is not a new one
    std::uint32_t held() const;
    // Whether the console reported a controller on the last read
    bool connected() const;

private:
    std::vector<hui::PadSample> samples;
};

} // namespace ps5::frontend
