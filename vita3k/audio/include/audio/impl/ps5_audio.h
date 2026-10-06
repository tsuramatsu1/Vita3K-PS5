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

#include "../state.h"

#include <cstdint>

// The console's own audio output. SDL has no driver for it, and the call it offers is the one the Vita offers: a
// buffer of a port's own length, which blocks until the hardware has taken it. That is what paces a game's audio
// thread here, as it does on a Vita
class Ps5AudioAdapter : public AudioAdapter {
public:
    explicit Ps5AudioAdapter(AudioState &audio_state)
        : AudioAdapter(audio_state) {}

    bool init() override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(AudioOutPort &out_port, const void *buffer) override;
    void set_volume(AudioOutPort &out_port, float volume) override;
};

struct Ps5AudioOutPort : public AudioOutPort {
    // The console's port, or -1 when it would not open: the port then keeps time but stays silent
    std::int32_t handle = -1;
    int channels = 2;

    ~Ps5AudioOutPort() override;
};
