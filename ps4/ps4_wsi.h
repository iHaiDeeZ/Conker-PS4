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

// PS4: Vulkan window system integration on VideoOut (ps4_wsi.cpp).

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

namespace ps4wsi {
// VideoOut's 1080p output (the system scales it for a 720p TV)
constexpr unsigned WIDTH = 1920;
constexpr unsigned HEIGHT = 1080;
} // namespace ps4wsi

// vkGetInstanceProcAddr of the statically linked RADV, with the PS4 surface and swapchain
// (VK_EXT_headless_surface + VK_KHR_swapchain on VideoOut) added.
PFN_vkGetInstanceProcAddr ps4_vk_get_instance_proc_addr();

// The app's VideoOut handle, opened on first use
int ps4wsi_video_handle();
// Flips submitted by swapchains so far
unsigned long long ps4wsi_flip_count();
