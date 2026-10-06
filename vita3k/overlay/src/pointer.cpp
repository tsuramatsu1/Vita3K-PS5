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

#include <overlay/pointer.h>

#include <algorithm>

namespace overlay {

namespace {
// Big enough to find on a television, small enough not to hide what is under it
constexpr int16_t SIZE = 14;
constexpr int16_t RING = 22;
} // namespace

pointer::pointer() {
    m_body.set_size(SIZE, SIZE);
    m_outline.set_size(RING, RING);
    min_refresh_duration_us = 16600;
    always_on_top = true;
}

void pointer::set_position(float x, float y, bool pressed) {
    const float clamped_x = std::clamp(x, 0.f, static_cast<float>(virtual_width));
    const float clamped_y = std::clamp(y, 0.f, static_cast<float>(virtual_height));
    if (clamped_x == m_x && clamped_y == m_y && pressed == m_pressed)
        return;

    m_x = clamped_x;
    m_y = clamped_y;
    m_pressed = pressed;
    needs_redraw.store(true, std::memory_order_relaxed);
}

compiled_resource pointer::get_compiled() {
    compiled_resource result;
    if (!visible)
        return result;

    // A ring around a dot: the ring keeps it visible over light and dark alike, and filling in on a press is the
    // only feedback a blind touchpad can give
    m_outline.set_pos(static_cast<int16_t>(m_x) - RING / 2, static_cast<int16_t>(m_y) - RING / 2);
    m_outline.back_color = m_pressed ? color4f{ 1.f, 1.f, 1.f, 0.55f } : color4f{ 0.f, 0.f, 0.f, 0.45f };
    result.add(m_outline.get_compiled());

    m_body.set_pos(static_cast<int16_t>(m_x) - SIZE / 2, static_cast<int16_t>(m_y) - SIZE / 2);
    m_body.back_color = m_pressed ? color4f{ 0.96f, 0.75f, 0.13f, 1.f } : color4f{ 1.f, 1.f, 1.f, 0.9f };
    result.add(m_body.get_compiled());

    return result;
}

} // namespace overlay
