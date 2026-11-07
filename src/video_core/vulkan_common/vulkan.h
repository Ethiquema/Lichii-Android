// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(__HAIKU__)
#define VK_USE_PLATFORM_XCB_KHR
#elif defined(__OPENORBIS__)
// No Vulkan on the PlayStation 4
#else
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include <vulkan/vulkan.h>

#ifndef VK_KHR_MAINTENANCE_1_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_1_EXTENSION_NAME "VK_KHR_maintenance1"
#endif
#ifndef VK_KHR_MAINTENANCE_2_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_2_EXTENSION_NAME "VK_KHR_maintenance2"
#endif
#ifndef VK_KHR_MAINTENANCE_3_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_3_EXTENSION_NAME "VK_KHR_maintenance3"
#endif
#ifndef VK_KHR_MAINTENANCE_4_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_4_EXTENSION_NAME "VK_KHR_maintenance4"
#endif
#ifndef VK_KHR_MAINTENANCE_5_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_5_EXTENSION_NAME "VK_KHR_maintenance5"
#endif
#ifndef VK_KHR_MAINTENANCE_6_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_6_EXTENSION_NAME "VK_KHR_maintenance6"
#endif
#ifndef VK_KHR_MAINTENANCE_7_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_7_EXTENSION_NAME "VK_KHR_maintenance7"
#endif
#ifndef VK_KHR_MAINTENANCE_8_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_8_EXTENSION_NAME "VK_KHR_maintenance8"
#endif
#ifndef VK_KHR_MAINTENANCE_9_EXTENSION_NAME
#   define VK_KHR_MAINTENANCE_9_EXTENSION_NAME "VK_KHR_maintenance9"
#endif

// Sanitize macros
#undef CreateEvent
#undef CreateSemaphore
#undef Always
#undef False
#undef None
#undef True

// "Catch-all" handle for both Android and.. the rest of platforms
struct VkSurfaceKHR_T;
