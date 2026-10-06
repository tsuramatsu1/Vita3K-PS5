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

// The AudioOut call order and its constants follow PS5 RetroArch's src/audio_ps5.cpp and ProsperoEden's
// pe/platform/audio_out.cpp (both GPL-3.0-or-later)

#include <audio/impl/ps5_audio.h>

#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <thread>

extern "C" {
std::int32_t sceAudioOutInit();
std::int32_t sceAudioOutOpen(std::int32_t user, std::int32_t type, std::int32_t index, std::uint32_t grain_frames,
    std::uint32_t rate, std::uint32_t format);
std::int32_t sceAudioOutOutput(std::int32_t handle, const void *samples);
std::int32_t sceAudioOutClose(std::int32_t handle);
std::int32_t sceAudioOutSetVolume(std::int32_t handle, std::int32_t flags, const std::int32_t *volumes);
}

namespace {

// The user who started the title, the main output, and signed 16-bit samples
constexpr std::int32_t ANY_USER = 0xff;
constexpr std::int32_t PORT_TYPE_MAIN = 0;
constexpr std::uint32_t FORMAT_S16_MONO = 0;
constexpr std::uint32_t FORMAT_S16_STEREO = 1;
// sceAudioOutInit answers this once it has already run, which is not a failure
constexpr std::uint32_t ALREADY_INITIALIZED = 0x8026000e;
// Both channels at once, and the console's own full scale
constexpr std::int32_t VOLUME_ALL_CHANNELS = 0xffffffff;
constexpr std::int32_t VOLUME_0DB = 32768;

} // namespace

Ps5AudioOutPort::~Ps5AudioOutPort() {
    if (handle >= 0)
        sceAudioOutClose(handle);
}

bool Ps5AudioAdapter::init() {
    const std::int32_t result = sceAudioOutInit();
    if (result != 0 && static_cast<std::uint32_t>(result) != ALREADY_INITIALIZED) {
        LOG_ERROR("sceAudioOutInit failed: 0x{:08X}", static_cast<std::uint32_t>(result));
        return false;
    }
    return true;
}

AudioOutPortPtr Ps5AudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    auto port = std::make_shared<Ps5AudioOutPort>();
    port->channels = nb_channels;
    port->len_microseconds = (static_cast<std::uint64_t>(nb_sample) * 1'000'000ULL) / freq;
    port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(std::int16_t));

    const std::uint32_t format = nb_channels == 1 ? FORMAT_S16_MONO : FORMAT_S16_STEREO;
    port->handle = sceAudioOutOpen(ANY_USER, PORT_TYPE_MAIN, 0, static_cast<std::uint32_t>(nb_sample),
        static_cast<std::uint32_t>(freq), format);
    if (port->handle < 0) {
        // The console takes only certain lengths and rates. A port it refuses still keeps the game's audio thread
        // in time, so the game runs on at the right speed with nothing to hear
        LOG_ERROR("sceAudioOutOpen refused {} channels at {} Hz in blocks of {}: 0x{:08X}", nb_channels, freq,
            nb_sample, static_cast<std::uint32_t>(port->handle));
    } else {
        LOG_INFO("Audio out: {} channels at {} Hz in blocks of {}", nb_channels, freq, nb_sample);
    }
    return port;
}

void Ps5AudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    Ps5AudioOutPort &port = static_cast<Ps5AudioOutPort &>(out_port);
    if (port.handle < 0) {
        // Nothing to play it with: wait out the block so the game keeps its own time
        std::this_thread::sleep_for(std::chrono::microseconds(port.len_microseconds));
        return;
    }

    // This blocks until the hardware has taken the block, which is what paces the game's audio thread
    const std::int32_t result = sceAudioOutOutput(port.handle, buffer);
    if (result < 0)
        LOG_ERROR_ONCE("sceAudioOutOutput failed: 0x{:08X}", static_cast<std::uint32_t>(result));
}

void Ps5AudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    Ps5AudioOutPort &port = static_cast<Ps5AudioOutPort &>(out_port);
    if (port.handle < 0)
        return;

    const std::int32_t level = static_cast<std::int32_t>(std::clamp(volume, 0.0f, 1.0f) * VOLUME_0DB);
    const std::int32_t volumes[8] = { level, level, level, level, level, level, level, level };
    sceAudioOutSetVolume(port.handle, VOLUME_ALL_CHANNELS, volumes);
}
