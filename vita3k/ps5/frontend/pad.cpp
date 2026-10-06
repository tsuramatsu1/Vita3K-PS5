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

#include "pad.h"

#include <platform/pad.h>

namespace ps5::frontend {

bool Pad::open() {
    return platform::open_pad();
}

// The platform layer's samples are the console's, which the UI kit's are a copy of
const std::vector<hui::PadSample> &Pad::read() {
    samples.clear();
    for (const platform::PadSample &source : platform::read_pad()) {
        hui::PadSample sample;
        sample.buttons = source.buttons;
        sample.left_x = source.left_x;
        sample.left_y = source.left_y;
        sample.right_x = source.right_x;
        sample.right_y = source.right_y;
        sample.l2 = source.l2;
        sample.r2 = source.r2;
        sample.connected = source.connected;
        sample.timestamp_us = source.timestamp_us;
        samples.push_back(sample);
    }
    return samples;
}

std::uint32_t Pad::held() const {
    return platform::current_pad().buttons;
}

bool Pad::connected() const {
    return platform::current_pad().connected;
}

} // namespace ps5::frontend
