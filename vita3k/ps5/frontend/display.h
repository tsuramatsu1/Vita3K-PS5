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

#include <volk.h>

#include <cstdint>
#include <vector>

namespace ps5::frontend {

// The front end's own Vulkan device on VideoOut: the one display the driver exposes (VK_KHR_display), a FIFO
// swapchain, one render pass and two frames in flight. It is released before the emulator starts its own
class Display {
public:
    static constexpr std::uint32_t FRAMES_IN_FLIGHT = 2;

    Display() = default;
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
    ~Display();

    bool create();

    // Waits for the frame slot and acquires an image; false when there is nothing to draw into
    bool begin_frame();
    // Begins the render pass on the acquired image, cleared to the colour given
    void begin_pass(const float clear[4]);
    void end_frame();

    VkPhysicalDevice physical_device_handle() const { return physical_device; }
    VkDevice device_handle() const { return device; }
    VkQueue queue_handle() const { return queue; }
    std::uint32_t queue_family_index() const { return queue_family; }
    VkRenderPass render_pass_handle() const { return render_pass; }
    VkCommandBuffer command_buffer() const { return frames[frame].command_buffer; }
    std::uint32_t frame_index() const { return frame; }
    std::uint32_t width() const { return extent.width; }
    std::uint32_t height() const { return extent.height; }

private:
    struct Frame {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkSemaphore image_acquired = VK_NULL_HANDLE;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    // One per swapchain image: an image is presented once its rendering is done
    std::vector<VkSemaphore> rendered;
    Frame frames[FRAMES_IN_FLIGHT];
    std::uint32_t frame = 0;
    std::uint32_t image = 0;

    bool create_device();
    bool create_swapchain();
};

} // namespace ps5::frontend
