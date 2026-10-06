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

// The UI kit's five system calls (and its splash call), which its own console layer would provide

#include <platform/ps5/system.hpp>

#include <util/log.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>

extern "C" {
int sceKernelUsleep(unsigned int microseconds);
void catchReturnFromMain(int status);
}

namespace hui::sys {

std::int64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void log(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    LOG_INFO("UI kit: {}", line);
}

// The platform layer hides it as the title starts
bool hide_splash_screen() {
    return true;
}

void sleep_us(std::uint32_t microseconds) {
    sceKernelUsleep(microseconds);
}

void park() {
    for (;;)
        sceKernelUsleep(100000);
}

// Asks the shell to close the title, as a return from main does
void quit() {
    catchReturnFromMain(0);
    park();
}

} // namespace hui::sys
