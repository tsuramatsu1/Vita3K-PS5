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

// Guest memory on the PS5. A title cannot mmap anonymous memory, so the guest range is reserved address space,
// backed by direct memory as it is committed (PS5_PayloadSDK's ps5platform/shm.h)

#include <cstddef>
#include <cstdint>

namespace mem::ps5 {

uint8_t *reserve(size_t bytes);
void release(uint8_t *base, size_t bytes);
// Makes [offset, offset + bytes) of the range readable and writable. Direct memory backs the range in 64 KiB
// chunks; the parts of a newly backed chunk outside the request are left inaccessible
bool commit(uint8_t *base, size_t offset, size_t bytes);

} // namespace mem::ps5
