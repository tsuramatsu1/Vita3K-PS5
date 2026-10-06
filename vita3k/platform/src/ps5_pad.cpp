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

// The DualSense, read with scePad: SDL has no driver for the console's own controller

#include <platform/pad.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>

extern "C" {
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(std::int32_t *user);
int scePadInit();
int scePadOpen(std::int32_t user, std::int32_t port_type, std::int32_t index, const void *params);
int scePadGetHandle(std::int32_t user, std::int32_t port_type, std::int32_t index);
int scePadRead(std::int32_t handle, void *samples, std::int32_t capacity);
}

namespace platform {

namespace {

// One finger as the console reports it
struct ConsoleTouch {
    std::uint16_t x, y;
    std::uint8_t id;
    std::uint8_t reserve[3];
};

// One sample as the console writes it (PS5_VulkanTemplate's ps5/src/platform.c, with the motion and touch block
// that sits between the sticks and the connection state filled in)
struct ConsoleSample {
    std::uint32_t buttons;
    std::uint8_t left_x, left_y, right_x, right_y, l2, r2;
    std::uint8_t padding[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    std::uint8_t touch_count;
    std::uint8_t touch_reserve[3];
    std::uint32_t touch_reserve1;
    ConsoleTouch touches[2];
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t remaining[15];
};
static_assert(sizeof(ConsoleSample) == 120, "the console's pad samples are 120 bytes");
static_assert(offsetof(ConsoleSample, touch_count) == 0x34, "the touch block starts at 0x34");
static_assert(offsetof(ConsoleSample, touches) == 0x3c, "the fingers sit at 0x3c");
static_assert(offsetof(ConsoleSample, connected) == 0x4c, "connection state sits at 0x4c");
static_assert(offsetof(ConsoleSample, timestamp_us) == 0x50, "the timestamp sits at 0x50");

// What the DualSense's touchpad spans, to place a finger across it from 0 to 1
constexpr float TOUCHPAD_WIDTH = 1920.0f;
constexpr float TOUCHPAD_HEIGHT = 1080.0f;

constexpr int SAMPLE_CAPACITY = 64;
// A title can start before the pad service has published the controller
constexpr int OPEN_ATTEMPTS = 10;
constexpr unsigned int OPEN_RETRY_US = 100000;

std::int32_t pad_handle = -1;
std::once_flag opened;
// The console's queue is drained by whoever reads first, so every read is serialised
std::mutex pad_mutex;
std::vector<PadSample> queued;
PadSample latest;

} // namespace

bool open_pad() {
    std::call_once(opened, [] {
        sceUserServiceInitialize(nullptr);
        std::int32_t user = -1;
        if (sceUserServiceGetInitialUser(&user) < 0 || scePadInit() < 0) {
            std::fprintf(stderr, "pad: no user or no pad service\n");
            return;
        }
        for (int attempt = 0; attempt < OPEN_ATTEMPTS && pad_handle < 0; attempt++) {
            pad_handle = scePadOpen(user, 0, 0, nullptr);
            if (pad_handle < 0 && attempt + 1 < OPEN_ATTEMPTS)
                sceKernelUsleep(OPEN_RETRY_US);
        }
        // A handle this process already holds for the user is reused
        if (pad_handle < 0)
            pad_handle = scePadGetHandle(user, 0, 0);
        if (pad_handle < 0)
            std::fprintf(stderr, "pad: could not open the controller of user %d\n", user);
    });
    return pad_handle >= 0;
}

namespace {

// Drains what the console has queued into queued, newest left in latest. The caller holds pad_mutex
void drain() {
    queued.clear();
    ConsoleSample raw[SAMPLE_CAPACITY];
    const int count = scePadRead(pad_handle, raw, SAMPLE_CAPACITY);
    for (int i = 0; i < count; i++) {
        PadSample sample;
        sample.buttons = raw[i].buttons;
        sample.left_x = raw[i].left_x;
        sample.left_y = raw[i].left_y;
        sample.right_x = raw[i].right_x;
        sample.right_y = raw[i].right_y;
        sample.l2 = raw[i].l2;
        sample.r2 = raw[i].r2;
        sample.connected = raw[i].connected != 0;
        sample.timestamp_us = raw[i].timestamp_us;
        sample.touch_count = std::min<int>(raw[i].touch_count, 2);
        for (int finger = 0; finger < sample.touch_count; finger++) {
            sample.touches[finger].x = raw[i].touches[finger].x / TOUCHPAD_WIDTH;
            sample.touches[finger].y = raw[i].touches[finger].y / TOUCHPAD_HEIGHT;
            sample.touches[finger].id = raw[i].touches[finger].id;
        }
        queued.push_back(sample);
        latest = sample;
    }
}

} // namespace

PadSample poll_pad() {
    if (!open_pad())
        return {};
    const std::lock_guard<std::mutex> lock(pad_mutex);
    drain();
    return latest;
}

PadSample current_pad() {
    const std::lock_guard<std::mutex> lock(pad_mutex);
    return latest;
}

const std::vector<PadSample> &read_pad() {
    if (!open_pad()) {
        queued.clear();
        return queued;
    }
    const std::lock_guard<std::mutex> lock(pad_mutex);
    drain();
    return queued;
}

} // namespace platform
