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
#include "ps5_host_memory.h"

#include <util/align.h>
#include <util/log.h>

#include <ps5platform/shm.h>

#include <sys/mman.h>

#include <mutex>
#include <vector>

namespace mem::ps5 {

namespace {

// Direct memory's allocation unit
constexpr size_t CHUNK_SIZE = 0x10000;

// Where emulators' guest arenas go: below the platform heap (0x20_0000_0000) and the Vulkan driver's device
// memory (0x40_0000_0000), outside the GPU window
void *const RESERVE_HINT = reinterpret_cast<void *>(0x1000000000ull);

// The backing of each chunk of the one guest range; a chunk is backed once and kept until the range is released
std::mutex chunks_mutex;
std::vector<ps5_shm> chunks;

} // namespace

uint8_t *reserve(size_t bytes) {
    void *base = nullptr;
    const int result = ps5_vrange_reserve(bytes, RESERVE_HINT, CHUNK_SIZE, &base);
    if (result != 0) {
        LOG_CRITICAL("Reserving {} bytes of guest address space failed: 0x{:X}", bytes, static_cast<uint32_t>(result));
        return nullptr;
    }

    const std::lock_guard<std::mutex> lock(chunks_mutex);
    chunks.assign(bytes / CHUNK_SIZE, ps5_shm{});
    return static_cast<uint8_t *>(base);
}

void release(uint8_t *base, size_t bytes) {
    const std::lock_guard<std::mutex> lock(chunks_mutex);
    for (size_t i = 0; i < chunks.size(); i++) {
        if (chunks[i].bytes == 0)
            continue;
        ps5_shm_unmap(base + i * CHUNK_SIZE, CHUNK_SIZE, PS5_SHM_KEEP_RESERVED);
        ps5_shm_destroy(&chunks[i]);
    }
    chunks.clear();
    ps5_vrange_release(base, bytes);
}

bool commit(uint8_t *base, size_t offset, size_t bytes) {
    const std::lock_guard<std::mutex> lock(chunks_mutex);
    const size_t first_chunk = align_down(offset, CHUNK_SIZE) / CHUNK_SIZE;
    const size_t end_chunk = align(offset + bytes, CHUNK_SIZE) / CHUNK_SIZE;
    for (size_t i = first_chunk; i < end_chunk; i++) {
        if (chunks[i].bytes != 0)
            continue;

        uint8_t *const chunk = base + i * CHUNK_SIZE;
        void *view = nullptr;
        int result = ps5_shm_create(CHUNK_SIZE, &chunks[i]);
        if (result == 0) {
            result = ps5_shm_map(&chunks[i], 0, CHUNK_SIZE, chunk, PS5_SHM_READ | PS5_SHM_WRITE, PS5_SHM_FIXED, &view);
            if (result != 0)
                ps5_shm_destroy(&chunks[i]);
        }
        if (result != 0) {
            chunks[i] = ps5_shm{};
            LOG_CRITICAL("Backing guest memory at 0x{:X} failed: 0x{:X}", i * CHUNK_SIZE, static_cast<uint32_t>(result));
            return false;
        }

        // Only the request is accessible, as uncommitted guest pages are elsewhere
        mprotect(chunk, CHUNK_SIZE, PROT_NONE);
    }

    if (mprotect(base + offset, bytes, PROT_READ | PROT_WRITE) != 0) {
        LOG_CRITICAL("Making guest memory at 0x{:X} accessible failed", offset);
        return false;
    }
    return true;
}

} // namespace mem::ps5
