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

#include "display.h"

#include <util/log.h>

#include <algorithm>

// The PS5 has no Vulkan loader: the driver (RADV) is linked into the executable and exports its ICD entry point
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

namespace ps5::frontend {

namespace {

bool check(VkResult result, const char *what) {
    if (result == VK_SUCCESS)
        return true;
    LOG_ERROR("{} failed: VkResult {}", what, static_cast<int>(result));
    return false;
}

} // namespace

Display::~Display() {
    if (device) {
        vkDeviceWaitIdle(device);
        for (Frame &slot : frames) {
            vkDestroyFence(device, slot.in_flight, nullptr);
            vkDestroySemaphore(device, slot.image_acquired, nullptr);
        }
        for (VkSemaphore semaphore : rendered)
            vkDestroySemaphore(device, semaphore, nullptr);
        for (VkFramebuffer framebuffer : framebuffers)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        for (VkImageView view : views)
            vkDestroyImageView(device, view, nullptr);
        vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyRenderPass(device, render_pass, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
}

bool Display::create() {
    volkInitializeCustom(vk_icdGetInstanceProcAddr);

    const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app_info.pApplicationName = "Vita3K";
    app_info.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(std::size(instance_extensions));
    instance_info.ppEnabledExtensionNames = instance_extensions;
    if (!check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance"))
        return false;
    volkLoadInstance(instance);

    // The console has one GPU and one display
    std::uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(instance, &count, &physical_device) < 0 || count == 0) {
        LOG_ERROR("No Vulkan device");
        return false;
    }
    VkDisplayPropertiesKHR display{};
    count = 1;
    if (vkGetPhysicalDeviceDisplayPropertiesKHR(physical_device, &count, &display) < 0 || count == 0) {
        LOG_ERROR("The Vulkan driver exposes no VideoOut display");
        return false;
    }
    std::vector<VkDisplayModePropertiesKHR> modes;
    vkGetDisplayModePropertiesKHR(physical_device, display.display, &count, nullptr);
    modes.resize(count);
    vkGetDisplayModePropertiesKHR(physical_device, display.display, &count, modes.data());
    if (modes.empty()) {
        LOG_ERROR("VideoOut has no modes");
        return false;
    }
    const VkDisplayModePropertiesKHR &mode = modes.front();

    VkDisplaySurfaceCreateInfoKHR surface_info{ VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
    surface_info.displayMode = mode.displayMode;
    surface_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surface_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surface_info.imageExtent = mode.parameters.visibleRegion;
    if (!check(vkCreateDisplayPlaneSurfaceKHR(instance, &surface_info, nullptr, &surface), "vkCreateDisplayPlaneSurfaceKHR"))
        return false;

    return create_device() && create_swapchain();
}

bool Display::create_device() {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < count && !found; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            queue_family = i;
            found = true;
        }
    }
    if (!found) {
        LOG_ERROR("No graphics queue presents to VideoOut");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(std::size(device_extensions));
    device_info.ppEnabledExtensionNames = device_extensions;
    if (!check(vkCreateDevice(physical_device, &device_info, nullptr, &device), "vkCreateDevice"))
        return false;
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (!check(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool), "vkCreateCommandPool"))
        return false;
    for (Frame &slot : frames) {
        VkCommandBufferAllocateInfo allocate_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate_info.commandPool = command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!check(vkAllocateCommandBuffers(device, &allocate_info, &slot.command_buffer), "vkAllocateCommandBuffers")
            || !check(vkCreateFence(device, &fence_info, nullptr, &slot.in_flight), "vkCreateFence")
            || !check(vkCreateSemaphore(device, &semaphore_info, nullptr, &slot.image_acquired), "vkCreateSemaphore"))
            return false;
    }
    return true;
}

bool Display::create_swapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);
    extent = capabilities.currentExtent;

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats.data());
    if (formats.empty()) {
        LOG_ERROR("VideoOut offers no surface format");
        return false;
    }
    // The kit's colours are sRGB values, written as they are
    const auto unorm = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR &candidate) {
        return candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM;
    });
    const VkSurfaceFormatKHR chosen = unorm != formats.end() ? *unorm : formats.front();
    format = chosen.format;

    std::uint32_t image_count = std::max(capabilities.minImageCount, 3u);
    if (capabilities.maxImageCount > 0)
        image_count = std::min(image_count, capabilities.maxImageCount);

    VkSwapchainCreateInfoKHR swapchain_info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = chosen.format;
    swapchain_info.imageColorSpace = chosen.colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // The only mode the display WSI offers
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain), "vkCreateSwapchainKHR"))
        return false;

    std::vector<VkImage> images;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    // The acquired image is written only once the presentation engine is done reading it
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    if (!check(vkCreateRenderPass(device, &pass_info, nullptr, &render_pass), "vkCreateRenderPass"))
        return false;

    for (VkImage swapchain_image : images) {
        VkImageViewCreateInfo view_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image = swapchain_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (!check(vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView"))
            return false;
        views.push_back(view);

        VkFramebufferCreateInfo framebuffer_info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &view;
        framebuffer_info.width = extent.width;
        framebuffer_info.height = extent.height;
        framebuffer_info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (!check(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer), "vkCreateFramebuffer"))
            return false;
        framebuffers.push_back(framebuffer);

        VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (!check(vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore"))
            return false;
        rendered.push_back(semaphore);
    }
    LOG_INFO("Front end on VideoOut at {}x{}, {} images", extent.width, extent.height, images.size());
    return true;
}

bool Display::begin_frame() {
    Frame &slot = frames[frame];
    vkWaitForFences(device, 1, &slot.in_flight, VK_TRUE, UINT64_MAX);
    const VkResult acquired = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, slot.image_acquired, VK_NULL_HANDLE, &image);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        LOG_ERROR("vkAcquireNextImageKHR failed: VkResult {}", static_cast<int>(acquired));
        return false;
    }
    vkResetFences(device, 1, &slot.in_flight);
    vkResetCommandBuffer(slot.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.command_buffer, &begin_info);
    return true;
}

void Display::begin_pass(const float clear[4]) {
    VkClearValue clear_value{};
    std::copy(clear, clear + 4, clear_value.color.float32);
    VkRenderPassBeginInfo pass_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass_begin.renderPass = render_pass;
    pass_begin.framebuffer = framebuffers[image];
    pass_begin.renderArea.extent = extent;
    pass_begin.clearValueCount = 1;
    pass_begin.pClearValues = &clear_value;
    vkCmdBeginRenderPass(frames[frame].command_buffer, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
}

void Display::end_frame() {
    Frame &slot = frames[frame];
    vkCmdEndRenderPass(slot.command_buffer);
    vkEndCommandBuffer(slot.command_buffer);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &slot.image_acquired;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &slot.command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &rendered[image];
    vkQueueSubmit(queue, 1, &submit_info, slot.in_flight);

    VkPresentInfoKHR present_info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &rendered[image];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image;
    vkQueuePresentKHR(queue, &present_info);

    frame = (frame + 1) % FRAMES_IN_FLIGHT;
}

} // namespace ps5::frontend
