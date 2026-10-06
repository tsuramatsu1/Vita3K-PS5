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

// The front end, drawn with BlackBearReloaded's ps5-homebrew-ui kit (PS5_VKHomebrewUI's Vulkan backend)

#include "frontend.h"

#include "display.h"
#include "pad.h"

#include <core/input.hpp>
#include <core/save_file.hpp>
#include <gfx/backdrop_spec.hpp>
#include <gfx/draw_list.hpp>
#include <gfx/font.hpp>
#include <gfx/vk/vk_renderer.hpp>
#include <ui/fonts.hpp>
#include <ui/theme.hpp>

#include <stb_image.h>

#include <platform/platform.h>

extern "C" {
std::int32_t sceAudioOutInit();
std::int32_t sceAudioOutOpen(std::int32_t user, std::int32_t type, std::int32_t index, std::uint32_t grain_frames,
    std::uint32_t rate, std::uint32_t format);
std::int32_t sceAudioOutOutput(std::int32_t handle, const void *samples);
std::int32_t sceAudioOutClose(std::int32_t handle);
}
#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ps5::frontend {

namespace {

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

constexpr const char *FONT_DIRECTORY = "/app0/assets/hui/fonts";
// In ui::Fonts order, as the kit bakes them
constexpr const char *FONT_FILES[6] = {
    "inter-regular.huifont",
    "inter-semibold.huifont",
    "montserrat-medium.huifont",
    "dejavu-sans-mono.huifont",
    "press-start-2p.huifont",
    "patrick-hand.huifont",
};
// PS5_VulkanTemplate's default
constexpr std::string_view THEME_ID = "tiles";

constexpr float MARGIN = 120.0f;
// The splash, as the shell shows it (from tools/ps5/make-title-art.py): held, then faded into the home screen
constexpr const char *SPLASH_IMAGE = "/app0/assets/home/splash.png";
constexpr float SPLASH_HOLD_US = 1200000.0f;
constexpr float SPLASH_FADE_US = 400000.0f;
// The library, as the PS Vita's home screen: ten bubbles to a page in rows of three, four and three
constexpr int BUBBLES_PER_PAGE = 10;
constexpr int BUBBLE_ROWS[3] = { 3, 4, 3 };
constexpr float BUBBLE_RADIUS = 136.0f;
constexpr float BUBBLE_FOCUS_SCALE = 1.1f;
constexpr float BUBBLE_PITCH = 450.0f;
constexpr float BUBBLE_TOP = 262.0f;
constexpr float BUBBLE_ROW_PITCH = 310.0f;
// The game's name under its bubble, over at most two lines
constexpr float LABEL_SIZE = 25.0f;
constexpr float LABEL_LINE = 29.0f;
constexpr int LABEL_LINES = 2;
constexpr float STATUS_BAR = 64.0f;
// The installer's list
constexpr const char *PARENT_ENTRY = "..";
constexpr std::uint64_t NOTICE_US = 4000000;
constexpr int INSTALL_ROWS = 9;
constexpr float INSTALL_ROW_HEIGHT = 66.0f;
// The ball a game's icon is baked onto, in pixels
constexpr int BUBBLE_TEXTURE = 512;
// The launch animation
constexpr float LAUNCH_US = 1100000.0f;
constexpr float LAUNCH_RADIUS = 760.0f;
constexpr float LAUNCH_TURNS = 1.5f;
constexpr std::size_t LOG_LINES = 4;
// The logo's own colours, which the progress bars and the highlights are drawn in
constexpr std::uint32_t LOGO_YELLOW = 0xf5c021;
constexpr std::uint32_t LOGO_ORANGE = 0xef7d22;
constexpr std::uint32_t LOGO_PINK = 0xe4326e;
constexpr std::size_t DETAIL_LINES = 6;
constexpr float DETAIL_ROW_HEIGHT = 46.0f;
constexpr float SETTINGS_ROW_HEIGHT = 72.0f;

// The controller in the status bar: lit while the console reports one, dimmed and struck through when it does not
void draw_controller(DrawList &out, float cx, float cy, bool connected) {
    const Color tint = connected ? Color::rgb(0xffffff) : Color::rgb(0xffffff, 0.35f);
    out.rounded_rect({ cx - 21.0f, cy - 11.0f, 42.0f, 22.0f }, 9.0f, tint);
    // The grips, and the sticks as two dark dots
    out.circle(cx - 19.0f, cy + 7.0f, 7.0f, tint);
    out.circle(cx + 19.0f, cy + 7.0f, 7.0f, tint);
    out.circle(cx - 8.0f, cy + 1.0f, 3.5f, Color::rgb(0x000000, 0.75f));
    out.circle(cx + 8.0f, cy + 1.0f, 3.5f, Color::rgb(0x000000, 0.75f));
    if (!connected)
        out.line(cx - 22.0f, cy + 13.0f, cx + 22.0f, cy - 13.0f, 3.0f, Color::rgb(0xe5484d));
}

// The face buttons, drawn rather than named. A hint writes them as {x} {o} {t} {s} and they are laid out inline
// with the words, because a picture of the button is what the player is looking at on the pad in their hands
constexpr float BUTTON_RADIUS = 15.0f;
constexpr float BUTTON_GAP = 9.0f;

void draw_button(DrawList &out, float cx, float cy, char kind) {
    constexpr float THICKNESS = 2.6f;
    const float r = BUTTON_RADIUS;
    out.circle(cx, cy, r, Color::rgb(0xffffff, 0.14f));
    out.ring(cx, cy, r, 1.6f, Color::rgb(0xffffff, 0.35f));

    const float inner = r * 0.52f;
    switch (kind) {
    case 'x': {
        const Color ink = Color::rgb(0x8fb6ef);
        out.line(cx - inner, cy - inner, cx + inner, cy + inner, THICKNESS, ink);
        out.line(cx - inner, cy + inner, cx + inner, cy - inner, THICKNESS, ink);
        break;
    }
    case 'o':
        out.ring(cx, cy, inner, THICKNESS, Color::rgb(0xe8697f));
        break;
    case 't': {
        const Color ink = Color::rgb(0x6fd6ae);
        out.line(cx, cy - inner, cx + inner, cy + inner * 0.75f, THICKNESS, ink);
        out.line(cx + inner, cy + inner * 0.75f, cx - inner, cy + inner * 0.75f, THICKNESS, ink);
        out.line(cx - inner, cy + inner * 0.75f, cx, cy - inner, THICKNESS, ink);
        break;
    }
    case 's': {
        const Color ink = Color::rgb(0xe49ad2);
        const float side = inner * 0.9f;
        out.line(cx - side, cy - side, cx + side, cy - side, THICKNESS, ink);
        out.line(cx + side, cy - side, cx + side, cy + side, THICKNESS, ink);
        out.line(cx + side, cy + side, cx - side, cy + side, THICKNESS, ink);
        out.line(cx - side, cy + side, cx - side, cy - side, THICKNESS, ink);
        break;
    }
    default:
        break;
    }
}

// Splits a hint into its glyphs and its words, so the whole line can be measured before it is placed
struct HintPiece {
    char button = 0; // 0 for a run of text
    std::string text;
};

std::vector<HintPiece> split_hint(std::string_view hint) {
    std::vector<HintPiece> pieces;
    std::string run;
    for (std::size_t i = 0; i < hint.size(); i++) {
        if (hint[i] == '{' && i + 2 < hint.size() && hint[i + 2] == '}') {
            if (!run.empty()) {
                pieces.push_back({ 0, run });
                run.clear();
            }
            pieces.push_back({ hint[i + 1], {} });
            i += 2;
            continue;
        }
        run += hint[i];
    }
    if (!run.empty())
        pieces.push_back({ 0, run });
    return pieces;
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A game's icon as the Vita draws it: a glass ball with the icon on its face. The icon is mapped onto the sphere by
// arc length, so it swells towards the viewer and packs together at the rim, and is then lit - shaded away from the
// light, a broad sheen over the top as on glass, a small highlight, and the light caught along the lower rim.
// Returns BUBBLE_TEXTURE squared RGBA pixels
std::vector<std::uint8_t> bake_bubble(const stbi_uc *source, int source_w, int source_h) {
    constexpr float BULGE = 0.80f;
    constexpr float AMBIENT = 0.62f;
    const float light[3] = { -0.4472f, -0.6149f, 0.8161f }; // normalized, from the upper left and in front
    const float half[3] = { -0.2144f, -0.2948f, 0.9314f }; // halfway to the viewer, for the highlight

    std::vector<std::uint8_t> out(static_cast<std::size_t>(BUBBLE_TEXTURE) * BUBBLE_TEXTURE * 4, 0);
    for (int y = 0; y < BUBBLE_TEXTURE; y++) {
        for (int x = 0; x < BUBBLE_TEXTURE; x++) {
            const float nx = (x + 0.5f) / BUBBLE_TEXTURE * 2.0f - 1.0f;
            const float ny = (y + 0.5f) / BUBBLE_TEXTURE * 2.0f - 1.0f;
            const float r = std::sqrt(nx * nx + ny * ny);
            std::uint8_t *pixel = &out[(static_cast<std::size_t>(y) * BUBBLE_TEXTURE + x) * 4];
            // One pixel of fade at the silhouette, so the ball has no jagged edge
            const float alpha = std::clamp((1.0f - r) * BUBBLE_TEXTURE * 0.5f, 0.0f, 1.0f);
            if (alpha <= 0.0f)
                continue;

            const float clamped = std::min(r, 1.0f);
            const float z = std::sqrt(std::max(0.0f, 1.0f - clamped * clamped));
            // Where on the icon this point of the sphere's surface lies
            const float arc = r > 1e-6f ? std::asin(clamped) / 1.5707963f / r : 1.0f;
            const float map = 1.0f + BULGE * (arc - 1.0f);
            const float u = std::clamp((nx * map * 0.5f + 0.5f) * (source_w - 1), 0.0f, source_w - 1.0f);
            const float v = std::clamp((ny * map * 0.5f + 0.5f) * (source_h - 1), 0.0f, source_h - 1.0f);

            const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
            const int x1 = std::min(x0 + 1, source_w - 1), y1 = std::min(y0 + 1, source_h - 1);
            const float fx = u - x0, fy = v - y0;
            float colour[3];
            for (int c = 0; c < 3; c++) {
                const float top = source[(y0 * source_w + x0) * 4 + c] * (1 - fx) + source[(y0 * source_w + x1) * 4 + c] * fx;
                const float bottom = source[(y1 * source_w + x0) * 4 + c] * (1 - fx) + source[(y1 * source_w + x1) * 4 + c] * fx;
                colour[c] = top * (1 - fy) + bottom * fy;
            }

            const float diffuse = std::max(0.0f, nx * light[0] + ny * light[1] + z * light[2]);
            const float shade = (AMBIENT + (1.0f - AMBIENT) * diffuse) * (0.45f + 0.55f * std::pow(z, 0.45f));

            const float ex = nx / 0.80f, ey = (ny + 0.42f) / 0.44f;
            const float sheen = (1.0f - smoothstep(0.35f, 1.0f, std::sqrt(ex * ex + ey * ey)))
                * std::clamp(0.45f - ny, 0.0f, 1.0f) * 0.42f;
            const float highlight = std::pow(std::max(0.0f, nx * half[0] + ny * half[1] + z * half[2]), 60.0f) * 0.45f;
            const float rim = std::pow(1.0f - z, 3.0f) * std::max(0.0f, ny) * 0.55f;
            const float rim_colour[3] = { 190.0f, 220.0f, 255.0f };

            for (int c = 0; c < 3; c++) {
                const float lit = colour[c] * shade + 255.0f * (sheen + highlight) + rim_colour[c] * rim;
                pixel[c] = static_cast<std::uint8_t>(std::clamp(lit, 0.0f, 255.0f));
            }
            pixel[3] = static_cast<std::uint8_t>(alpha * 255.0f);
        }
    }
    return out;
}

// The text, cut with an ellipsis to fit width
std::string fit(const hui::ui::FontRef &font, const std::string &text, float size, float width) {
    if (font.measure(text, size) <= width)
        return text;
    std::string cut = text;
    while (!cut.empty() && font.measure(cut + "...", size) > width)
        cut.pop_back();
    return cut + "...";
}

// The text broken between words over at most max_lines lines, the last cut with an ellipsis when the rest will not
// fit. A single word longer than the line is cut rather than left to overflow
std::vector<std::string> wrap(const hui::ui::FontRef &font, const std::string &text, float size, float width, int max_lines) {
    std::vector<std::string> words;
    for (std::istringstream stream(text); stream;) {
        std::string word;
        if (stream >> word)
            words.push_back(word);
    }

    std::vector<std::string> lines;
    std::string line;
    for (std::size_t i = 0; i < words.size(); i++) {
        const std::string candidate = line.empty() ? words[i] : line + " " + words[i];
        if (line.empty() || font.measure(candidate, size) <= width) {
            line = candidate;
            continue;
        }
        // The last line takes everything that is left, cut with an ellipsis
        if (static_cast<int>(lines.size()) + 1 == max_lines) {
            for (std::size_t rest = i; rest < words.size(); rest++)
                line += " " + words[rest];
            break;
        }
        lines.push_back(line);
        line = words[i];
    }
    lines.push_back(fit(font, line, size, width));
    return lines;
}

// The sound a game makes as it starts, in the spirit of a handheld's own: a soft chime that rises through three
// notes and opens out, under a breath of noise that swells and fades. Built from arithmetic rather than a recording
constexpr int CHIME_RATE = 48000;
constexpr int CHIME_GRAIN = 256;
constexpr float CHIME_SECONDS = 0.72f;
constexpr int CHIME_BLOCKS = static_cast<int>(CHIME_SECONDS * CHIME_RATE) / CHIME_GRAIN;
// A fifth and an octave above the root, arriving in turn
constexpr float CHIME_NOTES[3] = { 523.25f, 783.99f, 1046.50f };
constexpr float CHIME_ARRIVES[3] = { 0.00f, 0.09f, 0.18f };

void play_launch_sound() {
    std::thread([] {
        const std::int32_t started = sceAudioOutInit();
        if (started != 0 && static_cast<std::uint32_t>(started) != 0x8026000e)
            return;
        const std::int32_t port = sceAudioOutOpen(0xff, 0, 0, CHIME_GRAIN, CHIME_RATE, 1);
        if (port < 0)
            return;

        std::uint32_t noise = 0x2545f491;
        std::vector<std::int16_t> block(CHIME_GRAIN * 2);
        for (int b = 0; b < CHIME_BLOCKS; b++) {
            for (int i = 0; i < CHIME_GRAIN; i++) {
                const float t = static_cast<float>(b * CHIME_GRAIN + i) / CHIME_RATE;
                float sample = 0.0f;
                for (int n = 0; n < 3; n++) {
                    const float since = t - CHIME_ARRIVES[n];
                    if (since <= 0.0f)
                        continue;
                    // Struck, then let ring
                    const float strike = std::min(1.0f, since * 90.0f);
                    const float ring = std::exp(-since * 4.2f);
                    sample += std::sin(6.2831853f * CHIME_NOTES[n] * since) * strike * ring * 0.22f;
                }
                // The breath under it, swelling and gone by the end
                noise = noise * 1664525u + 1013904223u;
                const float hiss = (static_cast<float>(noise >> 9) / 4194304.0f - 1.0f);
                sample += hiss * std::exp(-t * 7.0f) * std::min(1.0f, t * 12.0f) * 0.05f;

                const float faded = sample * std::min(1.0f, (CHIME_SECONDS - t) * 6.0f);
                const auto value = static_cast<std::int16_t>(std::clamp(faded, -1.0f, 1.0f) * 26000.0f);
                block[i * 2] = value;
                block[i * 2 + 1] = value;
            }
            if (sceAudioOutOutput(port, block.data()) < 0)
                break;
        }
        sceAudioOutClose(port);
    }).detach();
}

std::uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Slot {
    float x;
    float y;
    int row;
};

// Where a page's bubbles sit: each row centred, the middle one a bubble wider
std::array<Slot, BUBBLES_PER_PAGE> bubble_slots() {
    std::array<Slot, BUBBLES_PER_PAGE> slots{};
    int i = 0;
    for (int row = 0; row < 3; row++) {
        const float width = (BUBBLE_ROWS[row] - 1) * BUBBLE_PITCH;
        for (int column = 0; column < BUBBLE_ROWS[row]; column++)
            slots[i++] = { (hui::gfx::kVirtualWidth - width) / 2.0f + column * BUBBLE_PITCH, BUBBLE_TOP + row * BUBBLE_ROW_PITCH, row };
    }
    return slots;
}

const std::array<Slot, BUBBLES_PER_PAGE> SLOTS = bubble_slots();

// The bubble the D-pad moves to from selected: the nearest one that way on the page, or across to the page above or
// below from the top and bottom rows. selected when there is none
int move_selection(int selected, int count, hui::Direction direction) {
    const int page = selected / BUBBLES_PER_PAGE;
    const Slot &from = SLOTS[selected % BUBBLES_PER_PAGE];
    const auto nearest = [&](int target_page, const auto &accept) {
        int best = -1;
        float best_distance = 0.0f;
        for (int slot = 0; slot < BUBBLES_PER_PAGE; slot++) {
            const int i = target_page * BUBBLES_PER_PAGE + slot;
            if (i >= count || i == selected || !accept(SLOTS[slot]))
                continue;
            const float dx = SLOTS[slot].x - from.x;
            const float dy = target_page == page ? SLOTS[slot].y - from.y : 0.0f;
            const float distance = dx * dx + dy * dy;
            if (best < 0 || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        return best;
    };
    int next = -1;
    switch (direction) {
    case hui::Direction::left:
        next = nearest(page, [&](const Slot &slot) { return slot.row == from.row && slot.x < from.x; });
        break;
    case hui::Direction::right:
        next = nearest(page, [&](const Slot &slot) { return slot.row == from.row && slot.x > from.x; });
        break;
    case hui::Direction::up:
        next = nearest(page, [&](const Slot &slot) { return slot.row == from.row - 1; });
        if (next < 0 && page > 0)
            next = nearest(page - 1, [](const Slot &slot) { return slot.row == 2; });
        break;
    case hui::Direction::down:
        next = nearest(page, [&](const Slot &slot) { return slot.row == from.row + 1; });
        if (next < 0)
            next = nearest(page + 1, [](const Slot &slot) { return slot.row == 0; });
        break;
    default:
        break;
    }
    return next >= 0 ? next : selected;
}

// The installer's reports, written by its thread and drawn by the front end's
class SharedProgress final : public InstallProgress {
public:
    void begin(const std::string &name, int index, int count) override {
        const std::lock_guard<std::mutex> lock(mutex);
        item = name;
        item_index = index;
        item_count = count;
        fraction = 0.0f;
    }

    void progress(float value) override {
        const std::lock_guard<std::mutex> lock(mutex);
        fraction = std::clamp(value, 0.0f, 1.0f);
    }

    void detail(const std::string &item, float fraction) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!items.empty() && items.back().name == item) {
            items.back().fraction = std::clamp(fraction, 0.0f, 1.0f);
            return;
        }
        // A new file means the one before it is done, however far it had been reported
        if (!items.empty())
            items.back().fraction = 1.0f;
        items.push_back({ item, std::clamp(fraction, 0.0f, 1.0f) });
        if (items.size() > DETAIL_LINES)
            items.pop_front();
    }

    void finished(const std::string &message, bool succeeded) override {
        const std::lock_guard<std::mutex> lock(mutex);
        log.push_back({ message, succeeded });
        if (log.size() > LOG_LINES)
            log.pop_front();
        fraction = 1.0f;
    }

    bool cancelled() const override {
        return stop.load(std::memory_order_relaxed);
    }

    struct Line {
        std::string text;
        bool succeeded;
    };

    std::atomic<bool> stop{ false };

    mutable std::mutex mutex;
    std::string item;
    int item_index = 0;
    int item_count = 0;
    float fraction = 0.0f;
    struct Item {
        std::string name;
        float fraction;
    };

    std::deque<Line> log;
    std::deque<Item> items;
};

class Ui {
public:
    ~Ui() {
        for (const auto &[path, texture] : icons) {
            if (texture)
                renderer.destroy_texture(texture);
        }
        for (const auto &[path, texture] : bubbles) {
            if (texture)
                renderer.destroy_texture(texture);
        }
        renderer.release();
    }

    bool init() {
        if (!display.create())
            return false;
        if (!pad.open())
            LOG_WARN("The front end has no controller");

        hui::gfx::VkRendererConfig config;
        config.physical_device = display.physical_device_handle();
        config.device = display.device_handle();
        config.queue = display.queue_handle();
        config.queue_family = display.queue_family_index();
        config.frames_in_flight = Display::FRAMES_IN_FLIGHT;
        config.render_pass = display.render_pass_handle();
        if (!renderer.init(config)) {
            LOG_ERROR("The UI renderer failed: VkResult {}", static_cast<int>(renderer.last_error()));
            return false;
        }

        hui::ui::FontRef *refs[6] = { &fonts.regular, &fonts.semibold, &fonts.display, &fonts.mono, &fonts.pixel, &fonts.hand };
        for (int i = 0; i < 6; i++) {
            const std::string path = std::string(FONT_DIRECTORY) + "/" + FONT_FILES[i];
            std::string data;
            if (!hui::save::read_file(path, &data) || !faces[i].load(data)) {
                LOG_ERROR("UI font {}: {}", path, faces[i].error());
                return false;
            }
            refs[i]->font = &faces[i];
            refs[i]->texture = renderer.create_font_texture(faces[i]);
        }

        const auto all = hui::ui::themes();
        const auto named = std::find_if(all.begin(), all.end(), [](const hui::ui::Theme &candidate) { return THEME_ID == candidate.id; });
        theme = named != all.end() ? &*named : &all.front();

        // A button already down as the title starts (the Cross that launched it) is not a press here
        hui::PadSample current;
        current.buttons = pad.held();
        current.connected = true;
        tracker.update(std::span<const hui::PadSample>(&current, 1), now_us());
        started_us = now_us();
        return true;
    }

    // Reads the controller, draws one frame with draw(list, input) and presents it
    template <typename Draw>
    bool frame(Draw &&draw) {
        const std::vector<hui::PadSample> &samples = pad.read();
        const hui::InputFrame input = tracker.update(samples, now_us());

        if (!display.begin_frame())
            return false;
        list.clear();
        draw(list, input);

        // The Vita's home screen: ribbons of light that drift across a sky going from near black to deep blue
        hui::gfx::BackdropSpec sky;
        sky.mode = hui::gfx::BackdropMode::waves;
        sky.colors[0] = Color::rgb(0x01030a);
        sky.colors[1] = Color::rgb(0x0a1c5e);
        sky.colors[2] = Color::rgb(0x2f6ad0);
        sky.colors[3] = Color::rgb(0x9fd0ff);
        sky.time = static_cast<float>(now_us() - started_us) / 1e6f;

        renderer.begin();
        renderer.backdrop(sky);
        renderer.draw(list);
        renderer.prepare(display.command_buffer(), display.frame_index());
        const float clear[4] = { theme->page.r, theme->page.g, theme->page.b, 1.0f };
        display.begin_pass(clear);
        renderer.draw(display.command_buffer(), static_cast<int>(display.width()), static_cast<int>(display.height()));
        display.end_frame();
        platform::hide_splash_screen();
        platform::keep_awake();
        return true;
    }

    // An image (a game's icon0.png, the home screen's art) as a texture, loaded once; 0 when it cannot be read
    std::uint32_t icon(const std::string &path) {
        const auto found = icons.find(path);
        if (found != icons.end())
            return found->second;
        int width = 0, height = 0, channels = 0;
        std::uint32_t texture = 0;
        if (stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4)) {
            texture = renderer.create_texture(width, height, pixels);
            stbi_image_free(pixels);
        }
        icons.emplace(path, texture);
        return texture;
    }

    // A game's icon baked onto its glass ball, once per game; 0 when the icon cannot be read
    std::uint32_t bubble(const std::string &path) {
        const auto found = bubbles.find(path);
        if (found != bubbles.end())
            return found->second;
        int width = 0, height = 0, channels = 0;
        std::uint32_t texture = 0;
        if (stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4)) {
            const std::vector<std::uint8_t> ball = bake_bubble(pixels, width, height);
            stbi_image_free(pixels);
            texture = renderer.create_texture(BUBBLE_TEXTURE, BUBBLE_TEXTURE, ball.data());
        }
        bubbles.emplace(path, texture);
        return texture;
    }

    void header(DrawList &out, std::string_view subtitle) const {
        hui::ui::text(out, fonts.display, "Vita3K", MARGIN, 150.0f, 76.0f, theme->text);
        hui::ui::text(out, fonts.regular, subtitle, MARGIN, 205.0f, 32.0f, theme->text_muted);
    }

    void hints(DrawList &out, std::string_view hint) const {
        constexpr float SIZE = 30.0f;
        const float y = hui::gfx::kVirtualHeight - 70.0f;
        const std::vector<HintPiece> pieces = split_hint(hint);

        float width = 0.0f;
        for (const HintPiece &piece : pieces)
            width += piece.button ? BUTTON_RADIUS * 2 + BUTTON_GAP : fonts.semibold.measure(piece.text, SIZE);

        float x = hui::gfx::kVirtualWidth - MARGIN - width;
        for (const HintPiece &piece : pieces) {
            if (piece.button) {
                draw_button(out, x + BUTTON_RADIUS, y - SIZE * 0.32f, piece.button);
                x += BUTTON_RADIUS * 2 + BUTTON_GAP;
            } else {
                hui::ui::text(out, fonts.semibold, piece.text, x, y, SIZE, theme->text_muted);
                x += fonts.semibold.measure(piece.text, SIZE);
            }
        }
    }

    Display display;
    Pad pad;
    hui::gfx::VkRenderer renderer;
    hui::gfx::Font faces[6];
    hui::ui::Fonts fonts;
    hui::InputTracker tracker;
    const hui::ui::Theme *theme = nullptr;
    DrawList list;
    std::map<std::string, std::uint32_t> icons;
    std::map<std::string, std::uint32_t> bubbles;
    std::uint64_t started_us = 0;
};

// Installs one package with its progress on screen, and leaves when it is done
void run_install(Ui &ui, const Installer &install, const Entry &file, const Destination &destination) {
    SharedProgress progress;
    std::atomic<bool> done = false;
    std::thread worker([&] {
        try {
            install(file, destination, progress);
        } catch (const std::exception &e) {
            LOG_ERROR("Installation of {} failed: {}", file.name, e.what());
            progress.finished(fmt::format("{} could not be installed: {}", file.name, e.what()), false);
        } catch (...) {
            LOG_ERROR("Installation of {} failed", file.name);
            progress.finished(fmt::format("{} could not be installed", file.name), false);
        }
        done = true;
    });

    const Rect panel{ MARGIN, 250.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 700.0f };
    std::uint64_t done_at = 0;
    bool confirming_cancel = false;
    while (true) {
        const bool finished = done;
        if (finished && !done_at)
            done_at = now_us();
        bool leave = false;
        ui.frame([&](DrawList &out, const hui::InputFrame &input) {
            const std::lock_guard<std::mutex> lock(progress.mutex);
            ui.header(out, finished ? "Installation finished" : "Installing");
            out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.45f));
            out.rounded_rect(panel, 28.0f, ui.theme->surface);

            const float x = panel.x + 56.0f;
            const float fill = finished ? 1.0f : progress.fraction;
            hui::ui::text(out, ui.fonts.semibold, fit(ui.fonts.semibold, finished ? "Done" : progress.item, 44.0f, panel.w - 260.0f),
                x, panel.y + 90.0f, 44.0f, ui.theme->text);
            hui::ui::text(out, ui.fonts.mono, fmt::format("{}%", static_cast<int>(fill * 100.0f + 0.5f)),
                panel.x + panel.w - 56.0f, panel.y + 90.0f, 40.0f, Color::rgb(LOGO_YELLOW), Align::right);

            const Rect track{ x, panel.y + 130.0f, panel.w - 112.0f, 18.0f };
            out.rounded_rect(track, 9.0f, ui.theme->surface_high);
            if (fill > 0.0f)
                out.gradient_rect_h({ track.x, track.y, track.w * fill, track.h }, 9.0f, Color::rgb(LOGO_YELLOW),
                    Color::rgb(LOGO_ORANGE));

            // What is being written now, under the files that went before it
            if (!progress.items.empty()) {
                const Rect detail{ x, panel.y + 180.0f, panel.w - 112.0f, DETAIL_LINES * DETAIL_ROW_HEIGHT + 20.0f };
                out.rounded_rect(detail, 14.0f, Color::rgb(0x000000, 0.35f));
                float line_y = detail.y + 34.0f;
                const std::size_t newest = progress.items.size() - 1;
                for (std::size_t i = 0; i < progress.items.size(); i++) {
                    const bool current = i == newest;
                    const SharedProgress::Item &item = progress.items[i];
                    hui::ui::text(out, current ? ui.fonts.semibold : ui.fonts.mono,
                        fit(ui.fonts.mono, item.name, 26.0f, detail.w - 150.0f), detail.x + 24.0f, line_y, 26.0f,
                        current ? ui.theme->text : ui.theme->text_muted);
                    hui::ui::text(out, ui.fonts.mono, fmt::format("{}%", static_cast<int>(item.fraction * 100.0f + 0.5f)),
                        detail.x + detail.w - 24.0f, line_y, 24.0f, ui.theme->text_muted, Align::right);
                    // This file's own bar, under its name
                    const Rect bar{ detail.x + 24.0f, line_y + 10.0f, detail.w - 48.0f, 5.0f };
                    out.rounded_rect(bar, 2.5f, Color::rgb(0xffffff, 0.12f));
                    if (item.fraction > 0.0f)
                        out.rounded_rect({ bar.x, bar.y, bar.w * item.fraction, bar.h }, 2.5f,
                            current ? Color::rgb(LOGO_YELLOW) : Color::rgb(LOGO_ORANGE, 0.55f));
                    line_y += DETAIL_ROW_HEIGHT;
                }
            }

            float y = panel.y + 180.0f + (progress.items.empty() ? 0.0f : DETAIL_LINES * DETAIL_ROW_HEIGHT + 56.0f) + 40.0f;
            for (const SharedProgress::Line &line : progress.log) {
                out.circle(x + 10.0f, y - 11.0f, 9.0f, line.succeeded ? Color::rgb(LOGO_YELLOW) : Color::rgb(LOGO_PINK));
                hui::ui::text(out, ui.fonts.regular, fit(ui.fonts.regular, line.text, 32.0f, panel.w - 150.0f),
                    x + 36.0f, y, 32.0f, ui.theme->text);
                y += 52.0f;
            }
            if (finished) {
                ui.hints(out, "{x} back");
                leave = input.is_pressed(hui::Action::confirm) || input.is_pressed(hui::Action::back)
                    || now_us() - done_at > 8000000;
            } else if (progress.stop.load(std::memory_order_relaxed)) {
                ui.hints(out, "Stopping...");
            } else if (confirming_cancel) {
                const Rect ask{ MARGIN + 80.0f, 420.0f, hui::gfx::kVirtualWidth - 2 * MARGIN - 160.0f, 260.0f };
                out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, 0.0f, Color::rgb(0x000000, 0.72f));
                out.shadow(ask, 28.0f, 40.0f, Color::rgb(0x000000, 0.5f));
                out.rounded_rect(ask, 28.0f, ui.theme->surface);
                hui::ui::text(out, ui.fonts.semibold, "Stop installing?", ask.x + 48.0f, ask.y + 80.0f, 40.0f, ui.theme->text);
                hui::ui::paragraph(out, ui.fonts.regular,
                    "What has been written so far will be removed and the installation will have to start again.",
                    ask.x + 48.0f, ask.y + 134.0f, 32.0f, ask.w - 96.0f, 44.0f, ui.theme->text_muted);
                ui.hints(out, "{x} stop     {o} keep installing");
                if (input.is_pressed(hui::Action::confirm))
                    progress.stop.store(true, std::memory_order_relaxed);
                if (input.is_pressed(hui::Action::confirm) || input.is_pressed(hui::Action::back))
                    confirming_cancel = false;
            } else {
                ui.hints(out, "{o} cancel");
                if (input.is_pressed(hui::Action::back))
                    confirming_cancel = true;
            }
        });
        if (leave)
            break;
    }
    worker.join();
}

// Where the game should be installed. Returns false when the player backs out. The firmware never asks: it belongs
// to the emulator, not to a game
bool ask_destination(Ui &ui, const std::vector<Destination> &destinations, const std::string &file_name, Destination &picked) {
    if (destinations.size() <= 1) {
        picked = destinations.empty() ? Destination{} : destinations.front();
        return true;
    }

    int selected = 0;
    bool answered = false, cancelled = false;
    while (!answered && !cancelled) {
        ui.frame([&](DrawList &out, const hui::InputFrame &input) {
            ui.header(out, "Install to");
            const Rect panel{ MARGIN, 300.0f, hui::gfx::kVirtualWidth - 2 * MARGIN,
                150.0f + destinations.size() * INSTALL_ROW_HEIGHT };
            out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.45f));
            out.rounded_rect(panel, 28.0f, ui.theme->surface);
            hui::ui::text(out, ui.fonts.regular, fit(ui.fonts.regular, file_name, 34.0f, panel.w - 112.0f),
                panel.x + 56.0f, panel.y + 70.0f, 34.0f, ui.theme->text_muted);

            const int count = static_cast<int>(destinations.size());
            if (input.nav == hui::Direction::down)
                selected = std::min(selected + 1, count - 1);
            else if (input.nav == hui::Direction::up)
                selected = std::max(selected - 1, 0);

            float y = panel.y + 110.0f;
            for (int i = 0; i < count; i++) {
                const Rect row{ panel.x + 28.0f, y, panel.w - 56.0f, INSTALL_ROW_HEIGHT - 8.0f };
                if (i == selected)
                    out.rounded_rect(row, 14.0f, ui.theme->surface_high);
                hui::ui::text(out, i == selected ? ui.fonts.semibold : ui.fonts.regular, destinations[i].name,
                    row.x + 28.0f, row.y + 44.0f, 34.0f, ui.theme->text);
                hui::ui::text(out, ui.fonts.mono, fit(ui.fonts.mono, destinations[i].path, 26.0f, row.w * 0.45f),
                    row.x + row.w - 28.0f, row.y + 44.0f, 26.0f, ui.theme->text_muted, Align::right);
                y += INSTALL_ROW_HEIGHT;
            }

            ui.hints(out, "{x} install here     {o} back");
            if (input.is_pressed(hui::Action::confirm)) {
                picked = destinations[selected];
                answered = true;
            } else if (input.is_pressed(hui::Action::back)) {
                cancelled = true;
            }
        });
    }
    return answered;
}

// The folder holding this one. The roots are reached from the folders just below them, as an empty path
std::string parent_of(const std::string &path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? std::string{} : path.substr(0, slash);
}

std::string human_size(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024)
        return fmt::format("{:.1f} GB", bytes / (1024.0 * 1024 * 1024));
    if (bytes >= 1024 * 1024)
        return fmt::format("{} MB", bytes / (1024 * 1024));
    return fmt::format("{} KB", std::max<std::uint64_t>(1, bytes / 1024));
}

// Browses for something to install: the install folder and the USB drives to start from, their folders to open, and
// the packages in them to install. Installing one comes back to the folder it came from, read again
void run_installer(Ui &ui, const Installer &install, const DirectoryLister &list_directory, const DestinationLister &list_destinations,
    const GamesFolderSetter &set_games_folder) {
    // The folder shown. An empty path is the list of places to start from, which the folders below it lead back to
    std::string current;
    std::vector<Entry> entries;
    int selected = 0;
    bool leave = false;
    // What was just done, shown under the list for a moment
    std::string notice;
    std::uint64_t notice_at = 0;

    // Moves to a folder, with the row holding the folder just left already picked out
    const auto show = [&](const std::string &path, const std::string &came_from) {
        current = path;
        entries = list_directory(current);
        if (!current.empty())
            entries.insert(entries.begin(), { PARENT_ENTRY, parent_of(current), true });
        selected = 0;
        for (int i = 0; i < static_cast<int>(entries.size()); i++) {
            if (entries[i].path == came_from && entries[i].name != PARENT_ENTRY) {
                selected = i;
                break;
            }
        }
    };
    const auto go_up = [&] {
        if (current.empty())
            leave = true;
        else
            show(parent_of(current), current);
    };
    show({}, {});

    while (!leave) {
        Entry chosen;
        ui.frame([&](DrawList &out, const hui::InputFrame &input) {
            ui.header(out, current.empty() ? "Install from" : current);
            const Rect panel{ MARGIN, 300.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 620.0f };
            out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.45f));
            out.rounded_rect(panel, 28.0f, ui.theme->surface);

            const int count = static_cast<int>(entries.size());
            if (count == 0) {
                hui::ui::paragraph(out, ui.fonts.regular,
                    current.empty()
                        ? "Nowhere to look. Put a .vpk, .zip or the firmware's .pup in /data/Vita3K/install, or plug "
                          "in a USB drive, then open this again."
                        : "This folder is empty.",
                    panel.x + 56.0f, panel.y + 90.0f, 36.0f, panel.w - 112.0f, 52.0f, ui.theme->text);
                ui.hints(out, current.empty() ? "{o} back" : "{o} up     {t} games");
                if (input.is_pressed(hui::Action::back))
                    go_up();
                else if (input.is_pressed(hui::Action::north))
                    leave = true;
                return;
            }

            if (input.nav == hui::Direction::down)
                selected = std::min(selected + 1, count - 1);
            else if (input.nav == hui::Direction::up)
                selected = std::max(selected - 1, 0);
            else if (input.is_pressed(hui::Action::page_next))
                selected = std::min(selected + INSTALL_ROWS, count - 1);
            else if (input.is_pressed(hui::Action::page_prev))
                selected = std::max(selected - INSTALL_ROWS, 0);

            // The window of rows that keeps the selected one on screen
            const int first = std::clamp(selected - INSTALL_ROWS / 2, 0, std::max(0, count - INSTALL_ROWS));
            float y = panel.y + 56.0f;
            for (int i = first; i < std::min(first + INSTALL_ROWS, count); i++) {
                const Entry &entry = entries[i];
                const Rect row{ panel.x + 28.0f, y, panel.w - 56.0f, INSTALL_ROW_HEIGHT - 8.0f };
                if (i == selected)
                    out.rounded_rect(row, 14.0f, ui.theme->surface_high);

                // A folder tab, the same with an arrow for the one above, or a page corner for a file
                const bool parent = entry.name == PARENT_ENTRY;
                const float mark = row.x + 30.0f;
                const Color mark_colour = parent ? Color::rgb(0xbfd8ff)
                    : entry.directory            ? Color::rgb(0xe8c35a)
                    : entry.installable          ? ui.theme->accent
                                                 : ui.theme->text_muted;
                if (entry.directory) {
                    out.rounded_rect({ mark - 16.0f, row.y + 18.0f, 14.0f, 6.0f }, 2.0f, mark_colour);
                    out.rounded_rect({ mark - 16.0f, row.y + 22.0f, 32.0f, 20.0f }, 4.0f, mark_colour);
                } else {
                    out.rounded_rect({ mark - 13.0f, row.y + 16.0f, 26.0f, 28.0f }, 4.0f, mark_colour);
                }
                if (parent) {
                    const Color arrow = Color::rgb(0x0a1020);
                    out.line(mark, row.y + 38.0f, mark, row.y + 27.0f, 3.0f, arrow);
                    out.line(mark - 6.0f, row.y + 33.0f, mark, row.y + 27.0f, 3.0f, arrow);
                    out.line(mark + 6.0f, row.y + 33.0f, mark, row.y + 27.0f, 3.0f, arrow);
                }

                const Color text = parent || entry.directory || entry.installable ? ui.theme->text : ui.theme->text_muted;
                hui::ui::text(out, i == selected ? ui.fonts.semibold : ui.fonts.regular,
                    parent ? "Parent folder" : fit(ui.fonts.regular, entry.name, 34.0f, row.w - 230.0f),
                    row.x + 64.0f, row.y + 44.0f, 34.0f, text);
                if (parent)
                    hui::ui::text(out, ui.fonts.semibold, "UP", row.x + row.w - 28.0f, row.y + 44.0f, 26.0f,
                        ui.theme->text_muted, Align::right);
                else if (!entry.directory)
                    hui::ui::text(out, ui.fonts.mono, human_size(entry.bytes), row.x + row.w - 28.0f, row.y + 44.0f,
                        28.0f, ui.theme->text_muted, Align::right);
                y += INSTALL_ROW_HEIGHT;
            }
            if (count > INSTALL_ROWS)
                hui::ui::text(out, ui.fonts.mono, fmt::format("{} of {}", selected + 1, count),
                    panel.x + panel.w - 28.0f, panel.y + panel.h + 44.0f, 28.0f, ui.theme->text_muted, Align::right);

            if (!notice.empty() && now_us() - notice_at < NOTICE_US)
                hui::ui::text(out, ui.fonts.regular, notice, panel.x + 28.0f, panel.y + panel.h + 44.0f, 28.0f,
                    ui.theme->accent);

            const Entry &entry = entries[selected];
            const bool parent = entry.name == PARENT_ENTRY;
            ui.hints(out, current.empty()
                    ? std::string{ "{x} open     {o} back" }
                    : fmt::format("{{x}} {}     {{s}} games folder     {{o}} up     {{t}} games",
                          parent ? "up" : entry.directory ? "open" : "install"));

            // Back to the list in one press, however deep the browser has gone
            if (input.is_pressed(hui::Action::north)) {
                leave = true;
                return;
            }

            // The folder to look in for games, as well as the ones always looked in
            if (!current.empty() && input.is_pressed(hui::Action::west)) {
                set_games_folder(current);
                notice = fmt::format("Games will be looked for in {}", current);
                notice_at = now_us();
            }

            if (input.is_pressed(hui::Action::confirm)) {
                if (parent)
                    go_up();
                else if (entry.directory)
                    show(entry.path, {});
                else if (entry.installable)
                    chosen = entry;
            } else if (input.is_pressed(hui::Action::back)) {
                go_up();
            }
        });

        if (!chosen.path.empty()) {
            // The firmware always goes to the emulator's own storage; a game can go to a USB drive instead
            Destination destination;
            const bool firmware = chosen.name.size() > 4
                && string_utils::tolower(chosen.name.substr(chosen.name.size() - 4)) == ".pup";
            if (firmware || ask_destination(ui, list_destinations(), chosen.name, destination))
                run_install(ui, install, chosen, destination);
            const std::string stay = entries[selected].path;
            show(current, stay);
        }
    }
}

// The settings worth reaching without a keyboard. Each is a value the player moves left and right through a fixed
// set of choices, because a console has no text entry and a wrong number here is worse than a coarse one
void run_settings(Ui &ui, const SettingsAccess &settings) {
    std::vector<Setting> values = settings.read();
    int selected = 0;
    // The first row drawn, which follows the selection rather than the other way round: there are more settings
    // than fit beside an explanation, and the explanation is the half that is worth reading
    int first_visible = 0;
    bool leave = false;

    while (!leave) {
        ui.frame([&](DrawList &out, const hui::InputFrame &input) {
            ui.header(out, "Settings");
            const Rect panel{ MARGIN, 280.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 640.0f };
            out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.45f));
            out.rounded_rect(panel, 28.0f, ui.theme->surface);

            if (values.empty()) {
                ui.hints(out, "{o} back");
                leave = input.is_pressed(hui::Action::back);
                return;
            }

            const int count = static_cast<int>(values.size());
            if (input.nav == hui::Direction::down)
                selected = std::min(selected + 1, count - 1);
            else if (input.nav == hui::Direction::up)
                selected = std::max(selected - 1, 0);
            else if (input.nav == hui::Direction::left || input.nav == hui::Direction::right) {
                Setting &setting = values[selected];
                const int step = input.nav == hui::Direction::right ? 1 : -1;
                setting.choice = std::clamp(setting.choice + step, 0, static_cast<int>(setting.choices.size()) - 1);
                settings.write(setting);
            }

            // The bottom of the panel belongs to the explanation, and the rows share what is left
            const float list_top = panel.y + 40.0f;
            const float explanation_top = panel.y + panel.h - 190.0f;
            const int visible = std::max(1, static_cast<int>((explanation_top - list_top) / SETTINGS_ROW_HEIGHT));
            first_visible = std::clamp(first_visible, std::max(0, selected - visible + 1), selected);
            first_visible = std::min(first_visible, std::max(0, count - visible));

            float y = list_top;
            for (int i = first_visible; i < std::min(count, first_visible + visible); i++) {
                const Setting &setting = values[i];
                const Rect row{ panel.x + 28.0f, y, panel.w - 56.0f, SETTINGS_ROW_HEIGHT - 10.0f };
                if (i == selected)
                    out.rounded_rect(row, 14.0f, ui.theme->surface_high);

                hui::ui::text(out, i == selected ? ui.fonts.semibold : ui.fonts.regular, setting.name, row.x + 28.0f,
                    row.y + 44.0f, 34.0f, ui.theme->text);
                // The chosen value between arrows, which say it can be moved through
                const float value_x = row.x + row.w - 28.0f;
                const std::string value = setting.choices[setting.choice];
                if (i == selected) {
                    const float width = ui.fonts.semibold.measure(value, 34.0f);
                    hui::ui::text(out, ui.fonts.regular, "<", value_x - width - 46.0f, row.y + 44.0f, 30.0f,
                        setting.choice > 0 ? Color::rgb(LOGO_YELLOW) : ui.theme->text_muted);
                    hui::ui::text(out, ui.fonts.regular, ">", value_x + 18.0f, row.y + 44.0f, 30.0f,
                        setting.choice + 1 < static_cast<int>(setting.choices.size()) ? Color::rgb(LOGO_YELLOW)
                                                                                      : ui.theme->text_muted);
                }
                hui::ui::text(out, ui.fonts.semibold, value, value_x, row.y + 44.0f, 34.0f,
                    i == selected ? Color::rgb(LOGO_YELLOW) : ui.theme->text_muted, Align::right);
                y += SETTINGS_ROW_HEIGHT;
            }

            // Where the selection sits in the whole list, for the rows that are off the top or the bottom
            if (count > visible) {
                const float track_h = visible * SETTINGS_ROW_HEIGHT - 10.0f;
                const float thumb_h = track_h * visible / count;
                const float thumb_y = list_top + track_h * first_visible / count;
                out.rounded_rect({ panel.x + panel.w - 18.0f, list_top, 4.0f, track_h }, 2.0f, ui.theme->surface_high);
                out.rounded_rect({ panel.x + panel.w - 18.0f, thumb_y, 4.0f, thumb_h }, 2.0f, Color::rgb(LOGO_YELLOW));
            }

            hui::ui::paragraph(out, ui.fonts.regular, values[selected].explanation, panel.x + 56.0f,
                explanation_top + 42.0f, 26.0f, panel.w - 112.0f, 34.0f, ui.theme->text_muted);

            ui.hints(out, "Left and right to change     {o} back");
            if (input.is_pressed(hui::Action::back))
                leave = true;
        });
    }
}

std::string run_library(Ui &ui, const std::vector<Game> &games, const GameRemover &remove_game, bool &install_requested,
    bool &settings_requested, bool &games_changed) {
    int selected = 0;
    std::string chosen;
    bool quit = false;
    // The launch, as the Vita plays it: the bubble spins like a coin while it grows to fill the screen
    std::uint64_t launch_started = 0;
    std::uint64_t sealed_notice = 0;
    // The game the player is being asked about before it is removed
    bool confirming_removal = false;
    while (chosen.empty() && !quit) {
        ui.frame([&](DrawList &out, const hui::InputFrame &input) {
            if (games.empty()) {
                ui.header(out, "No games installed");
                const Rect panel{ MARGIN, 300.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 300.0f };
                out.rounded_rect(panel, 28.0f, ui.theme->surface);
                hui::ui::paragraph(out, ui.fonts.regular,
                    "Press Triangle to install a .vpk, .zip or the firmware's .pup, from /data/Vita3K/install or "
                    "from a USB drive. A game's own folder, copied to a USB drive or to "
                    "/data/Vita3K/vita/ux0/app, is found here without installing.",
                    panel.x + 56.0f, panel.y + 90.0f, 36.0f, panel.w - 112.0f, 52.0f, ui.theme->text);
                ui.hints(out, "{t} install     Options settings     {o} quit");
                quit = input.is_pressed(hui::Action::back);
                if (input.is_pressed(hui::Action::menu)) {
                    settings_requested = true;
                    quit = true;
                }
                if (input.is_pressed(hui::Action::north)) {
                    install_requested = true;
                    quit = true;
                }
                return;
            }

            // The PS Vita's home screen: pages of ten glossy bubbles in rows of three, four and three, paged up
            // and down, with the page dots at the left and the status bar along the top
            const int count = static_cast<int>(games.size());
            const int pages = (count + BUBBLES_PER_PAGE - 1) / BUBBLES_PER_PAGE;
            const bool launching = launch_started != 0;
            if (!launching && input.nav != hui::Direction::none)
                selected = move_selection(selected, count, input.nav);

            // The game's ball; a width below the radius is that ball turned edge-on, as the launch animation spins it
            const auto draw_bubble = [&](const Game &game, float cx, float cy, float width, float radius) {
                const Rect bubble{ cx - width, cy - radius, 2 * width, 2 * radius };
                if (const std::uint32_t texture = ui.bubble(game.icon_path))
                    out.image(texture, bubble, hui::gfx::kFullUv, Color::rgb(0xffffff), 0.0f);
                else
                    out.rounded_rect(bubble, std::min(width, radius), Color::rgb(0x8a9bb8));
            };

            out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, STATUS_BAR }, 0.0f, Color::rgb(0x000000, 0.45f));
            hui::ui::text(out, ui.fonts.semibold, "Vita3K", 28.0f, 44.0f, 32.0f, Color::rgb(0xffffff));
            // The home glyph at the centre
            const float home_x = hui::gfx::kVirtualWidth / 2.0f;
            const float roof[6] = { home_x - 22.0f, 32.0f, home_x, 12.0f, home_x + 22.0f, 32.0f };
            out.polygon(roof, 3, Color::rgb(0xffffff));
            out.rounded_rect({ home_x - 15.0f, 31.0f, 30.0f, 20.0f }, 0.0f, Color::rgb(0xffffff));
            out.rounded_rect({ home_x - 5.0f, 39.0f, 10.0f, 12.0f }, 0.0f, Color::rgb(0x000000));
            const std::time_t now = std::time(nullptr);
            char clock[16] = {};
            std::strftime(clock, sizeof(clock), "%H:%M", std::localtime(&now));
            constexpr float CLOCK_SIZE = 36.0f;
            const float clock_right = hui::gfx::kVirtualWidth - 28.0f;
            hui::ui::text(out, ui.fonts.semibold, clock, clock_right, 44.0f, CLOCK_SIZE, Color::rgb(0xffffff), Align::right);
            draw_controller(out, clock_right - ui.fonts.semibold.measure(clock, CLOCK_SIZE) - 50.0f, STATUS_BAR / 2.0f,
                ui.pad.connected());

            const int shown_page = selected / BUBBLES_PER_PAGE;
            for (int slot = 0; slot < BUBBLES_PER_PAGE; slot++) {
                const int i = shown_page * BUBBLES_PER_PAGE + slot;
                if (i >= count)
                    break;
                const Game &game = games[i];
                const float cx = SLOTS[slot].x;
                const float cy = SLOTS[slot].y;
                const bool focused = i == selected;
                if (focused && launching)
                    continue; // drawn spinning, over everything
                const float radius = focused ? BUBBLE_RADIUS * BUBBLE_FOCUS_SCALE : BUBBLE_RADIUS;
                const Rect bubble{ cx - radius, cy - radius, 2 * radius, 2 * radius };
                if (focused)
                    out.glow(bubble, radius, 30.0f, Color::rgb(0xbfd8ff, 0.8f));
                out.shadow(bubble, radius, 24.0f, Color::rgb(0x000000, 0.55f));
                draw_bubble(game, cx, cy, radius, radius);
                if (game.sealed) {
                    // Still encrypted: dimmed, and crossed through
                    out.circle(cx, cy, radius, Color::rgb(0x05070f, 0.6f));
                    out.line(cx - radius * 0.5f, cy + radius * 0.5f, cx + radius * 0.5f, cy - radius * 0.5f, 7.0f,
                        Color::rgb(0xe5484d, 0.9f));
                }
                float label_y = cy + BUBBLE_RADIUS + 44.0f;
                for (const std::string &label : wrap(ui.fonts.semibold, game.title, LABEL_SIZE, BUBBLE_PITCH - 40.0f, LABEL_LINES)) {
                    hui::ui::text(out, ui.fonts.semibold, label, cx + 2.0f, label_y + 2.0f, LABEL_SIZE, Color::rgb(0x000000, 0.7f), Align::center);
                    hui::ui::text(out, ui.fonts.semibold, label, cx, label_y, LABEL_SIZE, Color::rgb(0xffffff), Align::center);
                    label_y += LABEL_LINE;
                }
            }

            // The page dots, the current one a white square
            if (pages > 1) {
                const float dots_h = (pages - 1) * 36.0f;
                for (int dot = 0; dot < pages; dot++) {
                    const float y = hui::gfx::kVirtualHeight / 2.0f - dots_h / 2.0f + dot * 36.0f;
                    if (dot == shown_page)
                        out.rounded_rect({ 28.0f, y - 10.0f, 20.0f, 20.0f }, 4.0f, Color::rgb(0xffffff));
                    else
                        out.circle(38.0f, y, 9.0f, Color::rgb(0x9aa0aa, 0.8f));
                }
            }
            if (launching) {
                const float t = std::min(1.0f, static_cast<float>(now_us() - launch_started) / LAUNCH_US);
                const float grow = t * t * (3.0f - 2.0f * t);
                const Slot &from = SLOTS[selected % BUBBLES_PER_PAGE];
                // The home screen dims behind the bubble, which moves to the centre as it grows
                out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, 0.0f, Color::rgb(0x000000, 0.75f * grow));
                const float cx = from.x + (hui::gfx::kVirtualWidth / 2.0f - from.x) * grow;
                const float cy = from.y + (hui::gfx::kVirtualHeight / 2.0f - from.y) * grow;
                const float start_radius = BUBBLE_RADIUS * BUBBLE_FOCUS_SCALE;
                const float radius = start_radius + (LAUNCH_RADIUS - start_radius) * grow;
                // A turn about the vertical axis is the bubble's width times the cosine of the angle
                const float width = std::max(0.02f, std::abs(std::cos(LAUNCH_TURNS * 2.0f * 3.14159265f * grow))) * radius;
                draw_bubble(games[selected], cx, cy, width, radius);
                // Then everything fades to black: the game takes over the screen
                const float fade = std::clamp((t - 0.7f) / 0.3f, 0.0f, 1.0f);
                out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, 0.0f, Color::rgb(0x000000, fade));
                if (t >= 1.0f)
                    chosen = games[selected].title_id;
                return;
            }

            if (confirming_removal) {
                const Game &game = games[selected];
                const Rect panel{ MARGIN, 340.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 300.0f };
                out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, 0.0f, Color::rgb(0x000000, 0.7f));
                out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.5f));
                out.rounded_rect(panel, 28.0f, ui.theme->surface);
                hui::ui::text(out, ui.fonts.semibold, "Delete this game?", panel.x + 56.0f, panel.y + 86.0f, 42.0f,
                    ui.theme->text);
                hui::ui::paragraph(out, ui.fonts.regular,
                    fmt::format("{} ({}) and everything in its folder will be removed. Saves kept elsewhere are left "
                                "alone. This cannot be undone.",
                        game.title, game.title_id),
                    panel.x + 56.0f, panel.y + 146.0f, 34.0f, panel.w - 112.0f, 46.0f, ui.theme->text_muted);
                ui.hints(out, "{x} delete     {o} keep");
                if (input.is_pressed(hui::Action::west)) {
                    remove_game(game.title_id);
                    // The list is read again either way: what did come off should stop being shown
                    games_changed = true;
                    confirming_removal = false;
                    quit = true; // the list is read again
                } else if (input.is_pressed(hui::Action::back) || input.is_pressed(hui::Action::confirm)) {
                    confirming_removal = false;
                }
                return;
            }

            // Why a game that is still encrypted did not start, until it is dismissed
            if (sealed_notice) {
                const Rect panel{ MARGIN, 330.0f, hui::gfx::kVirtualWidth - 2 * MARGIN, 340.0f };
                out.rounded_rect({ 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, 0.0f, Color::rgb(0x000000, 0.7f));
                out.shadow(panel, 28.0f, 40.0f, Color::rgb(0x000000, 0.5f));
                out.rounded_rect(panel, 28.0f, ui.theme->surface);
                hui::ui::text(out, ui.fonts.semibold, fit(ui.fonts.semibold, games[selected].title, 42.0f, panel.w - 112.0f),
                    panel.x + 56.0f, panel.y + 86.0f, 42.0f, ui.theme->text);
                hui::ui::paragraph(out, ui.fonts.regular,
                    "This game's files are still encrypted, so neither its icon nor the game itself can be read. "
                    "Copying a game's folder across only works once it has been decrypted. Install it from its .vpk "
                    "or .zip with Triangle, which decrypts it with its licence.",
                    panel.x + 56.0f, panel.y + 150.0f, 34.0f, panel.w - 112.0f, 48.0f, ui.theme->text_muted);
                ui.hints(out, "{x} back");
                if (input.is_pressed(hui::Action::confirm) || input.is_pressed(hui::Action::back))
                    sealed_notice = 0;
                return;
            }
            ui.hints(out, "{x} start     {t} install     {s} delete     Options settings     {o} quit");

            if (input.is_pressed(hui::Action::confirm)) {
                if (games[selected].sealed)
                    sealed_notice = now_us();
                else {
                    launch_started = now_us();
                    play_launch_sound();
                }
            } else if (input.is_pressed(hui::Action::back))
                quit = true;
            else if (input.is_pressed(hui::Action::north)) {
                install_requested = true;
                quit = true;
            } else if (input.is_pressed(hui::Action::west)) {
                confirming_removal = true;
            } else if (input.is_pressed(hui::Action::menu)) {
                settings_requested = true;
                quit = true;
            }
        });
    }
    return chosen;
}

void run_splash(Ui &ui) {
    const std::uint32_t splash = ui.icon(SPLASH_IMAGE);
    if (!splash)
        return;
    const std::uint64_t started = now_us();
    for (;;) {
        const float elapsed = static_cast<float>(now_us() - started);
        if (elapsed >= SPLASH_HOLD_US + SPLASH_FADE_US)
            break;
        const float opacity = 1.0f - std::clamp((elapsed - SPLASH_HOLD_US) / SPLASH_FADE_US, 0.0f, 1.0f);
        ui.frame([&](DrawList &out, const hui::InputFrame &) {
            out.image(splash, { 0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight }, hui::gfx::kFullUv,
                Color::rgb(0xffffff, opacity), 0.0f);
        });
    }
}

} // namespace

std::string run(const Installer &install, const DirectoryLister &list_directory, const DestinationLister &list_destinations,
    const GamesFolderSetter &set_games_folder, const GameRemover &remove_game, const SettingsAccess &settings,
    bool show_splash, const std::function<std::vector<Game>()> &list_games) {
    Ui ui;
    if (!ui.init())
        return {};
    if (show_splash)
        run_splash(ui);
    // The library and the installer in turn, until a game is picked or the player leaves
    while (true) {
        bool install_requested = false;
        bool settings_requested = false;
        bool games_changed = false;
        const std::string chosen
            = run_library(ui, list_games(), remove_game, install_requested, settings_requested, games_changed);
        if (games_changed)
            continue;
        if (settings_requested)
            run_settings(ui, settings);
        else if (install_requested)
            run_installer(ui, install, list_directory, list_destinations, set_games_folder);
        else
            return chosen;
    }
}

} // namespace ps5::frontend
