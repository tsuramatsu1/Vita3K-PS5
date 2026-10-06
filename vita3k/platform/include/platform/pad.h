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

#include <cstdint>
#include <vector>

namespace platform {

// One finger on the controller's touchpad, placed across it from 0 to 1 in each direction
struct PadTouch {
    float x = 0.0f;
    float y = 0.0f;
    std::uint8_t id = 0;
};

// The console's own controller, where SDL has no driver for it. The button bits are the console's, as pad_buttons
// below names them; the sticks run 0 to 255 with 128 at rest
struct PadSample {
    std::uint32_t buttons = 0;
    std::uint8_t left_x = 128;
    std::uint8_t left_y = 128;
    std::uint8_t right_x = 128;
    std::uint8_t right_y = 128;
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
    bool connected = false;
    std::uint64_t timestamp_us = 0;
    // The touchpad reports up to two fingers at once, which is what the Vita's own front panel reports too
    int touch_count = 0;
    PadTouch touches[2];
};

namespace pad_buttons {
constexpr std::uint32_t L3 = 0x00000002;
constexpr std::uint32_t R3 = 0x00000004;
constexpr std::uint32_t OPTIONS = 0x00000008;
constexpr std::uint32_t UP = 0x00000010;
constexpr std::uint32_t RIGHT = 0x00000020;
constexpr std::uint32_t DOWN = 0x00000040;
constexpr std::uint32_t LEFT = 0x00000080;
constexpr std::uint32_t L2 = 0x00000100;
constexpr std::uint32_t R2 = 0x00000200;
constexpr std::uint32_t L1 = 0x00000400;
constexpr std::uint32_t R1 = 0x00000800;
constexpr std::uint32_t TRIANGLE = 0x00001000;
constexpr std::uint32_t CIRCLE = 0x00002000;
constexpr std::uint32_t CROSS = 0x00004000;
constexpr std::uint32_t SQUARE = 0x00008000;
constexpr std::uint32_t TOUCHPAD = 0x00100000;
} // namespace pad_buttons

// Opens the controller of the user who started the title, once. False where there is no such controller, and on
// every platform whose controllers SDL already reads
bool open_pad();

// Reads the console and returns the controller's state now. What several of the emulator's threads call
PadSample poll_pad();

// The controller as the last read left it, without reading again, so an observer drawing a frame does not swallow
// the samples a reader is waiting for
PadSample current_pad();

// Every sample the console had queued, oldest first, for a caller that wants the presses between two frames. One
// thread at a time: the front end owns the controller while it runs
const std::vector<PadSample> &read_pad();

} // namespace platform
