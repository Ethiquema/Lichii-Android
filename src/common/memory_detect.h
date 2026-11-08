// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/**
 * Gets the memory the host can still hand out before paging starts.
 * @return Available physical memory in bytes, 0 when the host does not report it
 */
[[nodiscard]] u64 GetAvailablePhysicalMemory();

/**
 * Gets the host's limit on concurrent memory mappings.
 * @return vm.max_map_count on Linux, 0 when unknown
 */
[[nodiscard]] u64 GetMaxMapCount();

} // namespace Common
