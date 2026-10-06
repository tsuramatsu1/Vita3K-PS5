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

#include <overlay/element.h>
#include <overlay/overlay.h>

namespace overlay {

// Where a pointing device is aiming, drawn on top of the game. A touchscreen needs no such thing - a finger is
// already where the player is looking - but a controller's touchpad is a blind surface, so without this the
// player is guessing
struct pointer : public overlay {
    pointer();

    // In virtual screen space, 0..960 across and 0..544 down
    void set_position(float x, float y, bool pressed);

    compiled_resource get_compiled() override;

private:
    overlay_element m_body;
    overlay_element m_outline;
    float m_x = 0.f;
    float m_y = 0.f;
    bool m_pressed = false;
};

} // namespace overlay
