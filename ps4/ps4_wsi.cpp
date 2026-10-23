// Vita3K emulator project
// Copyright (C) 2026 Vita3K team (this file, adapted for Conker's Bad Fur Day: Recompiled for PS4)
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

// PS4: Vulkan window system integration on VideoOut (from M33's PS4VulkanWSI.cpp, itself from
// Resonance and Crema).
//
// RADV (DolphinPS4's GNM port) is linked into the executable and has no WSI. This file adds
// VK_EXT_headless_surface and VK_KHR_swapchain on top of it: the swapchain images are linear
// R8G8B8A8 or B8G8R8A8 images in host-visible memory (GPU address = CPU address on the PS4), registered as
// VideoOut buffers, and presenting queues a VideoOut flip behind the GPU work submitted so far
// (ac_ps4_submit_flip in the Mesa winsys). RT64 (plume) keeps using the standard surface/swapchain
// API through ps4_vk_get_instance_proc_addr(), which volk loads everything from.

#include "ps4_wsi.h"

#include "ps4_log.h"

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
// port/mesa/ac_ps4_drm.c: queues a VideoOut flip behind the submitted GPU work (GPU-side flip)
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);

namespace {

constexpr uint32_t MAX_IMAGES = 4;
constexpr VkDeviceSize FRAMEBUFFER_ALIGN = 2 << 20; // VideoOut buffer alignment

struct PS4Surface {
    uint32_t magic = 0x50533453;
};

struct PS4Swapchain {
    VkDevice device;
    uint32_t image_count;
    VkImage images[MAX_IMAGES]{};
    VkDeviceMemory memory[MAX_IMAGES]{};
    int buffer_set = -1;
    int first_buffer = 0;
    uint32_t next_image = 0;
    uint32_t flip_mode = ORBIS_VIDEO_OUT_FLIP_VSYNC;
};

VkInstance instance_ = VK_NULL_HANDLE;
VkPhysicalDevice physical_device = VK_NULL_HANDLE;
VkDevice device_ = VK_NULL_HANDLE; // for QueueSubmit
int video = -1;
OrbisKernelEqueue flip_queue;
uint64_t flips_submitted = 0;
// VideoOut's buffer slots 0-11, as 3 banks of MAX_IMAGES: a swapchain
// takes a free bank. A bank comes free only once VideoOut lets go of it: sceVideoOutUnregisterBuffers
// fails while one of its buffers is on screen (or still queued to be, the flips being the GPU's),
// which is just when a renderer destroys the old swapchain after making the new one. Such a set is
// parked (its memory kept: the screen may still be showing it) and released after the next flips.
constexpr int BANKS = 3;
bool bank_used[BANKS]{};
std::vector<PS4Swapchain *> retired;
std::mutex mutex;

template <typename T>
T device_fn(VkDevice device, const char *name) {
    return reinterpret_cast<T>(vk_common_GetDeviceProcAddr(device, name));
}

template <typename T>
T instance_fn(const char *name) {
    return reinterpret_cast<T>(vk_icdGetInstanceProcAddr(instance_, name));
}

bool open_video_out() {
    if (video >= 0)
        return true;
    video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (video < 0) {
        LOG_ERROR("PS4 WSI: sceVideoOutOpen failed: {:08X}", static_cast<uint32_t>(video));
        return false;
    }
    sceKernelCreateEqueue(&flip_queue, "conker flip");
    sceVideoOutAddFlipEvent(flip_queue, video, nullptr);
    return true;
}

// appends extensions to an enumeration result (count/properties two-call idiom)
VkResult append_extensions(std::vector<VkExtensionProperties> &list, std::initializer_list<std::pair<const char *, uint32_t>> extra,
    uint32_t *count, VkExtensionProperties *props) {
    for (const auto &[name, spec_version] : extra) {
        VkExtensionProperties p{};
        strncpy(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        p.specVersion = spec_version;
        list.push_back(p);
    }
    if (props == nullptr) {
        *count = static_cast<uint32_t>(list.size());
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(list.size()));
    memcpy(props, list.data(), n * sizeof(VkExtensionProperties));
    *count = n;
    return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

std::vector<const char *> without_extensions(uint32_t count, const char *const *names, std::initializer_list<const char *> drop) {
    std::vector<const char *> result;
    for (uint32_t i = 0; i < count; i++)
        if (std::none_of(drop.begin(), drop.end(), [&](const char *d) { return strcmp(names[i], d) == 0; }))
            result.push_back(names[i]);
    return result;
}

// --- instance level ---

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
    auto real = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    uint32_t n = 0;
    real(layer, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    real(layer, &n, list.data());
    list.resize(n);
    return append_extensions(list, { { VK_KHR_SURFACE_EXTENSION_NAME, 25 }, { VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, 1 } }, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *) {
    *count = 0; // no layers without a loader
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *instance) {
    auto real = reinterpret_cast<PFN_vkCreateInstance>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    VkInstanceCreateInfo copy = *info;
    auto names = without_extensions(info->enabledExtensionCount, info->ppEnabledExtensionNames,
        { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME });
    copy.enabledExtensionCount = static_cast<uint32_t>(names.size());
    copy.ppEnabledExtensionNames = names.data();
    copy.enabledLayerCount = 0;
    copy.ppEnabledLayerNames = nullptr;
    VkResult r = real(&copy, alloc, instance);
    if (r == VK_SUCCESS)
        instance_ = *instance;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    auto real = instance_fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    real(physical, layer, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    real(physical, layer, &n, list.data());
    list.resize(n);
    return append_extensions(list, { { VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70 } }, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *device) {
    auto real = instance_fn<PFN_vkCreateDevice>("vkCreateDevice");
    VkDeviceCreateInfo copy = *info;
    auto names = without_extensions(info->enabledExtensionCount, info->ppEnabledExtensionNames, { VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    copy.enabledExtensionCount = static_cast<uint32_t>(names.size());
    copy.ppEnabledExtensionNames = names.data();
    physical_device = physical;
    const VkResult r = real(physical, &copy, alloc, device);
    if (r == VK_SUCCESS)
        device_ = *device;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateHeadlessSurfaceEXT(VkInstance, const VkHeadlessSurfaceCreateInfoEXT *, const VkAllocationCallbacks *, VkSurfaceKHR *surface) {
    *surface = reinterpret_cast<VkSurfaceKHR>(new PS4Surface());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks *) {
    delete reinterpret_cast<PS4Surface *>(surface);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t queue_family, VkSurfaceKHR, VkBool32 *supported) {
    *supported = queue_family == 0 ? VK_TRUE : VK_FALSE; // the graphics queue family
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *caps) {
    // the images are linear R8G8B8A8 or B8G8R8A8: storage too where the GPU writes such images from compute
    // shaders (FSR's sharpening pass writes the swapchain image that way)
    VkFormatProperties format_props{};
    instance_fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_props);
    const bool storage = format_props.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    *caps = {};
    caps->minImageCount = 2;
    caps->maxImageCount = 3; // render into one while one is shown and one is queued
    caps->currentExtent = { ps4wsi::WIDTH, ps4wsi::HEIGHT };
    caps->minImageExtent = caps->currentExtent;
    caps->maxImageExtent = caps->currentExtent;
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (storage)
        caps->supportedUsageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps) {
    return GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info->surface, &caps->surfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count, VkSurfaceFormatKHR *formats) {
    // VideoOut A8B8G8R8 = R8G8B8A8 in memory (verified by DolphinPS4), A8R8G8B8 = B8G8R8A8: RT64
    // and recompui's swapchains are B8G8R8A8 (plume finds no format otherwise)
    const VkSurfaceFormatKHR supported[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    if (formats == nullptr) {
        *count = static_cast<uint32_t>(std::size(supported));
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(std::size(supported)));
    memcpy(formats, supported, n * sizeof(VkSurfaceFormatKHR));
    *count = n;
    return n < std::size(supported) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count, VkPresentModeKHR *modes) {
    const VkPresentModeKHR supported[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    if (modes == nullptr) {
        *count = static_cast<uint32_t>(std::size(supported));
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(std::size(supported)));
    memcpy(modes, supported, n * sizeof(VkPresentModeKHR));
    *count = n;
    return n < std::size(supported) ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- device level ---

uint32_t host_visible_memory_type(uint32_t type_bits) {
    VkPhysicalDeviceMemoryProperties props;
    instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical_device, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

void free_images(PS4Swapchain *sc) {
    auto destroy_image = device_fn<PFN_vkDestroyImage>(sc->device, "vkDestroyImage");
    auto free_memory = device_fn<PFN_vkFreeMemory>(sc->device, "vkFreeMemory");
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->images[i])
            destroy_image(sc->device, sc->images[i], nullptr);
        if (sc->memory[i])
            free_memory(sc->device, sc->memory[i], nullptr);
    }
    delete sc;
}

// Gives the swapchain's buffers back to VideoOut, then frees it; parks it if VideoOut won't yet.
void free_swapchain(PS4Swapchain *sc) {
    if (sc->buffer_set >= 0) {
        const int r = sceVideoOutUnregisterBuffers(video, sc->buffer_set);
        if (r < 0) {
            LOG_WARN("PS4 WSI: buffers {}+ still in use ({:08X}), released later", sc->first_buffer, static_cast<uint32_t>(r));
            retired.push_back(sc);
            return;
        }
        bank_used[sc->first_buffer / MAX_IMAGES] = false;
        sc->buffer_set = -1;
    }
    free_images(sc);
}

// Parked swapchains VideoOut has let go of since.
void release_retired() {
    for (size_t i = 0; i < retired.size();) {
        PS4Swapchain *sc = retired[i];
        if (sceVideoOutUnregisterBuffers(video, sc->buffer_set) < 0) {
            i++;
            continue;
        }
        LOG_INFO("PS4 WSI: buffers {}+ released", sc->first_buffer);
        bank_used[sc->first_buffer / MAX_IMAGES] = false;
        sc->buffer_set = -1;
        retired.erase(retired.begin() + i);
        free_images(sc);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *, VkSwapchainKHR *swapchain) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!open_video_out())
        return VK_ERROR_INITIALIZATION_FAILED;
    auto sc = new PS4Swapchain();
    sc->device = device;
    sc->image_count = std::clamp<uint32_t>(info->minImageCount, 2, MAX_IMAGES);
    // vsynced flips for mailbox too (Vita3K's choice): VideoOut can't replace a queued flip, and an
    // hsync flip shows a frame from the next line down, torn across the screen
    sc->flip_mode = info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? ORBIS_VIDEO_OUT_FLIP_HSYNC : ORBIS_VIDEO_OUT_FLIP_VSYNC;
    auto create_image = device_fn<PFN_vkCreateImage>(device, "vkCreateImage");
    auto get_requirements = device_fn<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements");
    auto allocate_memory = device_fn<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
    auto map_memory = device_fn<PFN_vkMapMemory>(device, "vkMapMemory");
    auto bind_memory = device_fn<PFN_vkBindImageMemory>(device, "vkBindImageMemory");
    auto get_layout = device_fn<PFN_vkGetImageSubresourceLayout>(device, "vkGetImageSubresourceLayout");
    void *addresses[MAX_IMAGES]{};
    uint32_t pitch = ps4wsi::WIDTH;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { ps4wsi::WIDTH, ps4wsi::HEIGHT, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        // storage as asked (FSR writes the image from a compute shader), else without it
        ici.usage = info->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = create_image(device, &ici, nullptr, &sc->images[i]);
        if (r != VK_SUCCESS && (ici.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            LOG_WARN("PS4 WSI: no storage swapchain images ({}), FSR can't work", static_cast<int>(r));
            ici.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
            r = create_image(device, &ici, nullptr, &sc->images[i]);
        }
        if (r != VK_SUCCESS) {
            LOG_ERROR("PS4 WSI: vkCreateImage failed: {}", static_cast<int>(r));
            free_swapchain(sc);
            return r;
        }
        VkMemoryRequirements req;
        get_requirements(device, sc->images[i], &req);
        // VideoOut wants 2 MiB aligned buffers: over-allocate, bind at an aligned offset
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size + FRAMEBUFFER_ALIGN;
        mai.memoryTypeIndex = host_visible_memory_type(req.memoryTypeBits);
        uint8_t *base = nullptr;
        r = allocate_memory(device, &mai, nullptr, &sc->memory[i]);
        if (r == VK_SUCCESS)
            r = map_memory(device, sc->memory[i], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&base));
        if (r != VK_SUCCESS) {
            LOG_ERROR("PS4 WSI: swapchain memory failed: {}", static_cast<int>(r));
            free_swapchain(sc);
            return r;
        }
        const uintptr_t base_address = reinterpret_cast<uintptr_t>(base);
        VkDeviceSize offset = ((base_address + FRAMEBUFFER_ALIGN - 1) & ~(FRAMEBUFFER_ALIGN - 1)) - base_address;
        offset = (offset + req.alignment - 1) & ~(req.alignment - 1);
        bind_memory(device, sc->images[i], sc->memory[i], offset);
        VkImageSubresource sub{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout layout;
        get_layout(device, sc->images[i], &sub, &layout);
        addresses[i] = base + offset + layout.offset;
        pitch = static_cast<uint32_t>(layout.rowPitch / 4);
    }
    // VideoOut's A8R8G8B8 (not in the OpenOrbis headers) is B8G8R8A8 in memory
    constexpr uint32_t PIXEL_FORMAT_A8R8G8B8_SRGB = 0x80000000u;
    const uint32_t pixel_format = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ? PIXEL_FORMAT_A8R8G8B8_SRGB : ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB;
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, pixel_format, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0,
        ps4wsi::WIDTH, ps4wsi::HEIGHT, pitch);
    // a free bank (the old swapchain is usually still on screen while this one registers)
    release_retired();
    sc->buffer_set = -1;
    int registered = -1;
    for (int bank = 0; bank < BANKS && sc->buffer_set < 0; bank++) {
        if (bank_used[bank])
            continue;
        sc->first_buffer = bank * static_cast<int>(MAX_IMAGES);
        registered = sceVideoOutRegisterBuffers(video, sc->first_buffer, addresses, static_cast<int>(sc->image_count), &attribute);
        if (registered >= 0) {
            sc->buffer_set = registered;
            bank_used[bank] = true;
        }
    }
    if (sc->buffer_set < 0) {
        LOG_ERROR("PS4 WSI: sceVideoOutRegisterBuffers failed: {:08X} ({} parked)", static_cast<uint32_t>(registered), retired.size());
        sc->buffer_set = -1;
        free_swapchain(sc);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOG_INFO("PS4 WSI: swapchain of {} images {}x{} pitch {}, buffers {}+", sc->image_count, ps4wsi::WIDTH, ps4wsi::HEIGHT, pitch, sc->first_buffer);
    *swapchain = reinterpret_cast<VkSwapchainKHR>(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *) {
    if (!swapchain)
        return;
    const std::lock_guard<std::mutex> lock(mutex);
    device_fn<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle")(device);
    free_swapchain(reinterpret_cast<PS4Swapchain *>(swapchain));
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images) {
    auto sc = reinterpret_cast<PS4Swapchain *>(swapchain);
    if (images == nullptr) {
        *count = sc->image_count;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min(*count, sc->image_count);
    memcpy(images, sc->images, n * sizeof(VkImage));
    *count = n;
    return n < sc->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

// present statistics, logged every 10 s
struct {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t flips = 0;
    uint64_t wait_ns = 0;
    uint32_t timeouts = 0;
    int64_t last_flip_arg = 0;
} stats;

// The image about to be rendered into must be off screen: with N images in rotation it was
// shown by flip n-N+1 and replaced on screen by flip n-N+2 (n = flips submitted).
void wait_image_off_screen(uint32_t image_count) {
    if (flips_submitted < image_count - 1)
        return;
    const int64_t needed = static_cast<int64_t>(flips_submitted - (image_count - 2));
    const auto t0 = std::chrono::steady_clock::now();
    bool done = false;
    for (int tries = 0; tries < 40 && !done; tries++) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(video, &status) < 0)
            break;
        stats.last_flip_arg = status.flipArg;
        if (status.flipArg >= needed) {
            done = true;
            break;
        }
        OrbisKernelEvent ev;
        int got = 0;
        OrbisKernelUseconds timeout = 5000;
        sceKernelWaitEqueue(flip_queue, &ev, 1, &got, &timeout);
    }
    if (!done)
        stats.timeouts++;
    stats.wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

void count_present() {
    stats.flips++;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - stats.start).count();
    if (secs < 10.0)
        return;
    LOG_INFO("PS4 WSI: {:.1f} flips/s, acquire waits {:.0f} ms/s, {} timeouts, flipArg {} of {} submitted",
        stats.flips / secs, stats.wait_ns / 1e6 / secs, stats.timeouts, stats.last_flip_arg, flips_submitted);
    stats.start = now;
    stats.flips = 0;
    stats.wait_ns = 0;
    stats.timeouts = 0;
}

VkQueue graphics_queue(VkDevice device) {
    VkQueue queue = VK_NULL_HANDLE;
    device_fn<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(device, 0, 0, &queue);
    return queue;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t, VkSemaphore semaphore, VkFence fence, uint32_t *index) {
    auto sc = reinterpret_cast<PS4Swapchain *>(swapchain);
    wait_image_off_screen(sc->image_count);
    *index = sc->next_image;
    sc->next_image = (sc->next_image + 1) % sc->image_count;
    // The image is free now (waited for above). The acquire semaphore is never signalled: there is one
    // in-order queue, and QueueSubmit below drops semaphores (an empty submit signalling one loses the
    // device on the PS4 - Crema, Resonance). A fence, if asked for, is signalled by an empty submit.
    (void)semaphore;
    if (fence) {
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        const VkResult r = device_fn<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(graphics_queue(device), 1, &si, fence);
        if (r != VK_SUCCESS)
            LOG_ERROR("PS4 WSI: acquire signal submit failed: {}", static_cast<int>(r));
        return r;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        auto sc = reinterpret_cast<PS4Swapchain *>(info->pSwapchains[i]);
        // the wait semaphores were never signalled (QueueSubmit drops them): one queue, in order, the
        // rendering was submitted before this flip
        const uint32_t buffer = static_cast<uint32_t>(sc->first_buffer) + info->pImageIndices[i];
        const int64_t arg = static_cast<int64_t>(flips_submitted + 1);
        // the flip goes to the GPU behind the frame's commands: no CPU wait for the GPU or vblank
        const uint32_t flip_mode = sc->flip_mode;
        int r = ac_ps4_submit_flip(video, buffer, flip_mode, arg);
        const bool gpu_flip = r == 0;
        if (!gpu_flip) {
            // fallback: wait for the GPU on the CPU, then flip
            device_fn<PFN_vkQueueWaitIdle>(sc->device, "vkQueueWaitIdle")(queue);
            r = sceVideoOutSubmitFlip(video, static_cast<int>(buffer), flip_mode, arg);
        }
        if (r < 0) {
            LOG_ERROR("PS4 WSI: flip failed: {:08X}", static_cast<uint32_t>(r));
            result = VK_ERROR_SURFACE_LOST_KHR;
        } else {
            flips_submitted++;
            count_present();
            if (!retired.empty()) {
                const std::lock_guard<std::mutex> lock(mutex);
                release_retired();
            }
            // the PS4 keeps its boot splash (the app's picture) over everything until the app
            // says it has a picture of its own
            if (flips_submitted == 1)
                sceSystemServiceHideSplashScreen();
            if (flips_submitted <= 3)
                LOG_INFO("PS4 WSI: flip {} of buffer {} ({})", flips_submitted, buffer, gpu_flip ? "GPU-side" : "CPU wait");
        }
        if (info->pResults)
            info->pResults[i] = r < 0 ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
    }
    return result;
}

// Submits without their semaphores: on the PS4 binary semaphores only lose the device, and with
// one in-order queue (and the CPU wait for the image in AcquireNextImageKHR) they add nothing.
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    static PFN_vkQueueSubmit real = nullptr;
    if (!real)
        real = device_fn<PFN_vkQueueSubmit>(device_, "vkQueueSubmit");
    VkSubmitInfo stripped[8];
    std::vector<VkSubmitInfo> many;
    VkSubmitInfo *copy = stripped;
    if (count > 8) {
        many.resize(count);
        copy = many.data();
    }
    for (uint32_t i = 0; i < count; i++) {
        copy[i] = submits[i];
        copy[i].waitSemaphoreCount = 0;
        copy[i].pWaitSemaphores = nullptr;
        copy[i].pWaitDstStageMask = nullptr;
        copy[i].signalSemaphoreCount = 0;
        copy[i].pSignalSemaphores = nullptr;
    }
    return real(queue, count, copy, fence);
}

PFN_vkVoidFunction intercept(const char *name) {
#define PS4WSI_ENTRY(fn)               \
    if (strcmp(name, "vk" #fn) == 0) \
    return reinterpret_cast<PFN_vkVoidFunction>(&fn)
    PS4WSI_ENTRY(EnumerateInstanceExtensionProperties);
    PS4WSI_ENTRY(EnumerateInstanceLayerProperties);
    PS4WSI_ENTRY(CreateInstance);
    PS4WSI_ENTRY(EnumerateDeviceExtensionProperties);
    PS4WSI_ENTRY(CreateDevice);
    PS4WSI_ENTRY(CreateHeadlessSurfaceEXT);
    PS4WSI_ENTRY(DestroySurfaceKHR);
    PS4WSI_ENTRY(GetPhysicalDeviceSurfaceSupportKHR);
    PS4WSI_ENTRY(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    PS4WSI_ENTRY(GetPhysicalDeviceSurfaceCapabilities2KHR);
    PS4WSI_ENTRY(GetPhysicalDeviceSurfaceFormatsKHR);
    PS4WSI_ENTRY(GetPhysicalDeviceSurfacePresentModesKHR);
    PS4WSI_ENTRY(CreateSwapchainKHR);
    PS4WSI_ENTRY(DestroySwapchainKHR);
    PS4WSI_ENTRY(GetSwapchainImagesKHR);
    PS4WSI_ENTRY(AcquireNextImageKHR);
    PS4WSI_ENTRY(QueuePresentKHR);
    PS4WSI_ENTRY(QueueSubmit);
#undef PS4WSI_ENTRY
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name) {
    // vulkan.hpp's DispatchLoaderDynamic::init(device) looks vkGetDeviceProcAddr up again through
    // itself: RADV's would then resolve the swapchain functions, and it has none (all null)
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    if (auto fn = intercept(name))
        return fn;
    return vk_common_GetDeviceProcAddr(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    if (auto fn = intercept(name))
        return fn;
    return vk_icdGetInstanceProcAddr(instance, name);
}

} // namespace

PFN_vkGetInstanceProcAddr ps4_vk_get_instance_proc_addr() {
    return &GetInstanceProcAddr;
}

// Waits for the display's next vblank (59.94 a second, as on the Vita), for the emulated vblanks
// (display.cpp): a game's frames then keep their place against the vsynced flips instead of drifting
// through them (a frame shown a vblank late every 17 s or so). False when it can't: the caller
// keeps time itself. A missed vblank, or a display not at 60 Hz (the first 120 vblanks timed), turns
// this off for good: a game must not slow down.
extern "C" bool ps4_wait_vblank(unsigned timeout_us) {
    static OrbisKernelEqueue queue = 0;
    static int ready = 0; // 0 not set up yet, 1 set up, -1 not available
    static int timed = 0;
    static std::chrono::steady_clock::time_point first;
    if (ready == 0) {
        const int handle = ps4wsi_video_handle();
        if (handle < 0)
            return false;
        // (the SDK declares it without its parameters)
        using AddVblankEvent = int32_t (*)(OrbisKernelEqueue, int32_t, void *);
        const int created = sceKernelCreateEqueue(&queue, "conker vblank");
        const int added = created < 0 ? created : reinterpret_cast<AddVblankEvent>(&sceVideoOutAddVblankEvent)(queue, handle, nullptr);
        if (created < 0 || added < 0) {
            LOG_WARN("PS4 WSI: no vblank events ({:08X}), the emulated vblanks keep their own time", static_cast<uint32_t>(created < 0 ? created : added));
            ready = -1;
            return false;
        }
        LOG_INFO("PS4 WSI: the emulated vblanks follow the display's");
        ready = 1;
    }
    if (ready < 0)
        return false;
    OrbisKernelEvent event;
    int got = 0;
    OrbisKernelUseconds timeout = timeout_us;
    const int waited = sceKernelWaitEqueue(queue, &event, 1, &got, &timeout);
    if (waited >= 0 && got > 0) {
        if (timed <= 120) {
            const auto now = std::chrono::steady_clock::now();
            if (timed == 0)
                first = now;
            else if (timed == 120) {
                const double rate = 120.0 / std::chrono::duration<double>(now - first).count();
                if (rate < 58.0 || rate > 61.0) {
                    LOG_WARN("PS4 WSI: the display gives {:.2f} vblanks a second, the emulated vblanks keep their own time", rate);
                    ready = -1;
                    return false;
                }
                LOG_INFO("PS4 WSI: the display gives {:.2f} vblanks a second", rate);
            }
            timed++;
        }
        return true;
    }
    constexpr int ETIMEDOUT_ERROR = static_cast<int>(0x8002003C);
    if (waited == ETIMEDOUT_ERROR || (waited >= 0 && got < 1)) {
        LOG_WARN("PS4 WSI: no vblank event in {} us, the emulated vblanks keep their own time from now on", timeout_us);
        ready = -1;
    }
    return false;
}

// Whether the last flip submitted still waits for its vblank, grace_us after asking (frame
// generation: that vblank's image then isn't made, rather than the render thread waiting a vblank
// for a free swapchain image at every vblank, as v00.38's did once two flips were queued)
extern "C" bool ps4_flip_pending(unsigned grace_us) {
    if (video < 0 || flips_submitted == 0)
        return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(grace_us);
    for (;;) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(video, &status) < 0)
            return false;
        if (status.flipArg >= static_cast<int64_t>(flips_submitted))
            return false;
        if (std::chrono::steady_clock::now() >= until)
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

int ps4wsi_video_handle() {
    const std::lock_guard<std::mutex> lock(mutex);
    return open_video_out() ? video : -1;
}

unsigned long long ps4wsi_flip_count() {
    return flips_submitted;
}
