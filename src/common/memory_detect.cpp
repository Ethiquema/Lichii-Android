// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || (defined(__FreeBSD__) && !defined(__OPENORBIS__))
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include "common/memory_detect.h"

#include <cstdio>
#include <cstring>

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__) && !defined(__OPENORBIS__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

// Reads the first unsigned integer out of a procfs file, 0 when unavailable.
#ifdef __linux__
static u64 ReadProcFsU64(const char* path) {
    std::FILE* const file = std::fopen(path, "re");
    if (file == nullptr) {
        return 0;
    }
    u64 value{};
    const int parsed = std::fscanf(file, "%lu", &value);
    std::fclose(file);
    return parsed == 1 ? value : 0;
}
#endif

u64 GetAvailablePhysicalMemory() {
#ifdef __linux__
    // MemAvailable: an estimate of how much memory can be handed out without swapping.
    std::FILE* const file = std::fopen("/proc/meminfo", "re");
    if (file == nullptr) {
        return 0;
    }
    u64 available_kib{};
    char line[256]{};
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::sscanf(line, "MemAvailable: %lu kB", &available_kib) == 1) {
            break;
        }
    }
    std::fclose(file);
    return available_kib << 10;
#else
    return 0;
#endif
}

u64 GetMaxMapCount() {
#ifdef __linux__
    return ReadProcFsU64("/proc/sys/vm/max_map_count");
#else
    return 0;
#endif
}

} // namespace Common
