// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#ifdef HAS_HYPERVISOR
#include "core/arm/hypervisor/hv_vm.h"
#endif
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_download_area.h"

namespace Core::Memory {

u32& UnmappedReadFlag() {
    thread_local u32 flag{};
    return flag;
}

static inline bool AddressSpaceContains(const Common::PageTable& table, const Common::ProcessAddress addr, const std::size_t size) {
    const Common::ProcessAddress max_addr = 1ULL << table.GetAddressSpaceBits();
    return addr + size >= addr && addr + size <= max_addr;
}

// Implementation class used to keep the specifics of the memory subsystem hidden
// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl {
    explicit Impl(Core::System& system_) : system{system_} {}

    void SetCurrentPageTable(Kernel::KProcess& process) {
        current_page_table = &process.GetPageTable().GetImpl();

        if (process.IsApplication() && Settings::IsFastmemEnabled()) {
            current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer();
        } else {
            current_page_table->fastmem_arena = nullptr;
        }
        host_buffer = std::addressof(system.DeviceMemory().buffer);
#endif

#ifdef HAS_HYPERVISOR
        // EDEN_NX2_HYPERVISOR=1: application code runs natively in a VM whose page tables mirror
        // this one from the start, so every mapping below reaches them.
        // (The process's 64-bit flag is only set after this runs; a 64-bit address space,
        // 36 or 39 bits, tells the same.)
        if (Core::Hv::IsRequested() && process.IsApplication() &&
            current_page_table->GetAddressSpaceBits() >= 36 && system.IsMulticore()) {
            auto& buffer = system.DeviceMemory().buffer;
            Core::Hv::Vm::Get().Attach(*current_page_table,
                                       current_page_table->GetAddressSpaceBits(),
                                       buffer.BackingBasePointer(), buffer.BackingSize());
        }
#endif
    }

    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size, Common::PhysicalAddress target, Common::MemoryPermission perms, bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}", GetInteger(target));
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, target, Common::PageType::Memory);

        if (current_page_table->fastmem_arena) {
            host_buffer->Map(GetInteger(base), GetInteger(target) - DramMemoryMap::Base, size, perms, separate_heap);
        }
    }

    void UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size, bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0, Common::PageType::Unmapped);

        if (current_page_table->fastmem_arena) {
            host_buffer->Unmap(GetInteger(base), size, separate_heap);
        }
    }

    void ProtectRegion(Common::PageTable& page_table, VAddr vaddr, u64 size,
                       Common::MemoryPermission perms) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((vaddr & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", vaddr);

#ifdef HAS_HYPERVISOR
        if (auto* vm = HypervisorFor(page_table)) {
            vm->Protect(vaddr, size, perms);
        }
#endif

        if (!current_page_table->fastmem_arena) {
            return;
        }

        u64 protect_bytes = 0, protect_begin = 0;
        for (u64 addr = vaddr; addr < vaddr + size; addr += YUZU_PAGESIZE) {
            const Common::PageType page_type = current_page_table->entries[addr >> YUZU_PAGEBITS].ptr.Type();
            switch (page_type) {
            case Common::PageType::RasterizerCachedMemory:
                if (protect_bytes > 0) {
                    host_buffer->Protect(protect_begin, protect_bytes, perms);
                    protect_bytes = 0;
                }
                break;
            default:
                if (protect_bytes == 0)
                    protect_begin = addr;
                protect_bytes += YUZU_PAGESIZE;
            }
        }

        if (protect_bytes > 0) {
            host_buffer->Protect(protect_begin, protect_bytes, perms);
        }
    }

    [[nodiscard]] u8* GetPointerFromRasterizerCachedMemory(u64 vaddr) const {
        Common::PhysicalAddress const paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].addr;
        if (paddr)
            return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
        return {};
    }

    [[nodiscard]] u8* GetPointerFromDebugMemory(u64 vaddr) const {
        const Common::PhysicalAddress paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].addr;
        if (paddr != 0)
            return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
        return {};
    }

    u8 Read8(const Common::ProcessAddress addr) {
        return Read<u8>(addr);
    }

    u16 Read16(const Common::ProcessAddress addr) {
        if ((addr & 1) == 0) {
            return Read<u16_le>(addr);
        } else {
            const u32 a{Read<u8>(addr)};
            const u32 b{Read<u8>(addr + sizeof(u8))};
            return static_cast<u16>((b << 8) | a);
        }
    }

    u32 Read32(const Common::ProcessAddress addr) {
        if ((addr & 3) == 0) {
            return Read<u32_le>(addr);
        } else {
            const u32 a{Read16(addr)};
            const u32 b{Read16(addr + sizeof(u16))};
            return (b << 16) | a;
        }
    }

    [[nodiscard]] inline u8* GetPointerFromDebugMemory(u64 vaddr) const {
        auto const paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].addr;
        return paddr ? system.DeviceMemory().GetPointer<u8>(paddr + vaddr) : nullptr;
    }

    bool WriteExclusive8(const Common::ProcessAddress addr, const u8 data, const u8 expected) {
        return WriteExclusive<u8>(addr, data, expected);
    }

    bool WriteExclusive16(const Common::ProcessAddress addr, const u16 data, const u16 expected) {
        return WriteExclusive<u16_le>(addr, data, expected);
    }

    bool WriteExclusive32(const Common::ProcessAddress addr, const u32 data, const u32 expected) {
        return WriteExclusive<u32_le>(addr, data, expected);
    }

    bool WriteExclusive64(const Common::ProcessAddress addr, const u64 data, const u64 expected) {
        return WriteExclusive<u64_le>(addr, data, expected);
    }

    std::string ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
        std::string string;
        string.reserve(max_length);
        for (std::size_t i = 0; i < max_length; ++i) {
            const char c = Read<s8>(vaddr);
            if (c == '\0') {
                break;
            }
            string.push_back(c);
            ++vaddr;
        }
        string.shrink_to_fit();
        return string;
    }

    template<typename F1, typename F2, typename F3>
    inline bool WalkBlock(const Common::ProcessAddress addr, const std::size_t size, F1&& on_unmapped, F2&& on_memory, F3&& on_rasterizer) {
        std::size_t offset = 0;
        if (!AddressSpaceContains(*current_page_table, addr, size)) {
            on_unmapped(offset, size, addr);
            return false;
        }
        std::size_t remaining_size = size;
        std::size_t page_index = addr >> YUZU_PAGEBITS;
        std::size_t page_offset = addr & YUZU_PAGEMASK;
        bool user_accessible = true;
        while (remaining_size != 0) {
            const std::size_t copy_amount = (std::min)(std::size_t(YUZU_PAGESIZE) - page_offset, remaining_size);
            const auto current_vaddr = u64((page_index << YUZU_PAGEBITS) + page_offset);
            const auto [pointer, type] = current_page_table->entries[page_index].ptr.PointerType();
            switch (type) {
            case Common::PageType::Unmapped: {
                user_accessible = false;
                on_unmapped(offset, copy_amount, current_vaddr);
                break;
            }
            case Common::PageType::Memory: {
                u8* mem_ptr = reinterpret_cast<u8*>(pointer + page_offset + (page_index << YUZU_PAGEBITS));
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::DebugMemory: {
                u8* const mem_ptr = GetPointerFromDebugMemory(current_vaddr);
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromRasterizerCachedMemory(current_vaddr);
                on_rasterizer(current_vaddr, offset, copy_amount, host_ptr);
                break;
            }
            default:
                UNREACHABLE();
            }
            page_index++;
            page_offset = 0;
            offset += copy_amount;
            remaining_size -= copy_amount;
        }
        return user_accessible;
    }

    bool ReadBlockImpl(const Common::ProcessAddress addr, void* buffer, const std::size_t size, bool unsafe) {
        return WalkBlock(addr, size,
        [addr, size, &buffer](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
            std::memset(reinterpret_cast<u8*>(buffer) + offset, 0, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        });
    }

    [[nodiscard]] inline const u8* GetSpan(const VAddr addr, const std::size_t size) const noexcept {
        return (current_page_table->entries[addr >> YUZU_PAGEBITS].block == current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].block) ? GetPointerSilent(addr) : nullptr;
    }
    [[nodiscard]] inline u8* GetSpan(const VAddr addr, const std::size_t size) noexcept {
        return (current_page_table->entries[addr >> YUZU_PAGEBITS].block == current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].block) ? GetPointerSilent(addr) : nullptr;
    }

    bool WriteBlockImpl(const Common::ProcessAddress addr, const void* buffer, const std::size_t size, bool unsafe) {
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        });
    }

    bool ZeroBlock(const Common::ProcessAddress addr, const std::size_t size) {
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [=](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memset(ptr, 0, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            std::memset(ptr, 0, copy_amount);
        });
    }

    bool CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr, const std::size_t size) {
        return WalkBlock(dest_addr, size,
        [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(src_addr), size);
            ZeroBlock(dest_addr + offset, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        });
    }

    template <typename Callback>
    Result PerformCacheOperation(Common::ProcessAddress dest_addr, std::size_t size, Callback&& cb) {
        bool had_unmapped = false;
        WalkBlock(dest_addr, size,
            [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory, "Unmapped @ {:#018X}", GetInteger(current_vaddr));
                had_unmapped = true;
            },
            [](const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) {},
            [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) { cb(current_vaddr, offset, copy_amount); }
        );
        return had_unmapped ? Kernel::ResultInvalidCurrentMemory : ResultSuccess;
    }

    Result InvalidateDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc ivac: Invalidate to point of coherency
            // GPU flush -> CPU invalidate
            HandleRasterizerDownload(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result StoreDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc cvac: Store to point of coherency
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result FlushDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc civac: Store to point of coherency, and invalidate from cache
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    void MarkRegionDebug(u64 vaddr, u64 size, bool debug) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }

        if (current_page_table->fastmem_arena) {
            const auto perm{debug ? Common::MemoryPermission{}
                                  : Common::MemoryPermission::ReadWrite};
            host_buffer->Protect(vaddr, size, perm);
        }

        // Iterate over a contiguous CPU address space, marking/unmarking the region.
        // The region is at a granularity of CPU pages.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            const Common::PageType page_type = current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Type();
            if (debug) {
                // Switch page type to debug if now debug
                switch (page_type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::DebugMemory:
                    // Page is already marked.
                    break;
                case Common::PageType::Memory:
                    current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Store(0, Common::PageType::DebugMemory);
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to non-debug if now non-debug
                switch (page_type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as non-debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::Memory:
                    // Don't mess with already non-debug or rasterizer memory.
                    break;
                case Common::PageType::DebugMemory: {
                    u8* const pointer = GetPointerFromDebugMemory(vaddr & ~YUZU_PAGEMASK);
                    current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Store(uintptr_t(pointer) - (vaddr & ~YUZU_PAGEMASK), Common::PageType::Memory);
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    void RasterizerMarkRegionCached(u64 vaddr, u64 size, bool cached) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }

#ifdef HAS_HYPERVISOR
        // Native code writes guest memory directly: protect what the GPU caches track, as fastmem
        // does, so those writes fault and reach the caches.
        if (auto* vm = HypervisorFor(*current_page_table)) {
            vm->MarkCached(vaddr, size, cached,
                           cached && Settings::values.use_reactive_flushing.GetValue());
        }
#endif

        if (current_page_table->fastmem_arena) {
            Common::MemoryPermission perm{};
            // EDEN_NX2_NO_REACTIVE=1 drops the read traps on Switch 2 programs. Opt-in: without
            // them a CPU read of GPU-written memory can see stale data.
            static const bool nx2_no_reactive = [] {
                const char* value = std::getenv("EDEN_NX2_NO_REACTIVE");
                return value != nullptr && value[0] == '1';
            }();
            const bool reactive_flushing =
                Settings::values.use_reactive_flushing.GetValue() &&
                !(nx2_no_reactive && Settings::CurrentProgramIsSwitch2());
            if (!reactive_flushing || !cached) {
                perm |= Common::MemoryPermission::Read;
            }
            if (!cached) {
                perm |= Common::MemoryPermission::Write;
            }
            host_buffer->Protect(vaddr, size, perm);
            if (cached && perm != Common::MemoryPermission{}) {
                ProtectArmedPages(vaddr, size);
            }
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            const Common::PageType page_type= current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Type();
            if (cached) {
                // Switch page type to cached if now cached
                switch (page_type) {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Store(0, Common::PageType::RasterizerCachedMemory);
                    break;
                case Common::PageType::RasterizerCachedMemory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already marked as cached.
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to uncached if now uncached
                switch (page_type) {
                case Common::PageType::Unmapped: // NOLINT(bugprone-branch-clone)
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already unmarked as cached.
                    break;
                case Common::PageType::RasterizerCachedMemory: {
                    if (u8* const pointer = GetPointerFromRasterizerCachedMemory(vaddr & ~YUZU_PAGEMASK); pointer == nullptr) {
                        // It's possible that this function has been called while updating the
                        // pagetable after unmapping a VMA. In that case the underlying VMA will no
                        // longer exist, and we should just leave the pagetable entry blank.
                        current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Store(0, Common::PageType::Unmapped);
                    } else {
                        current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Store(uintptr_t(pointer) - (vaddr & ~YUZU_PAGEMASK), Common::PageType::Memory);
                    }
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    /// Read traps replace reactive flushing's blanket read protection of every cached page: only
    /// pages the GPU writes are protected, and each one only until its first read
    /// (EDEN_NX2_READ_TRAPS=0 turns them off). Under NCE every fault is taken as a write, so they
    /// are not used there.
    bool ReadTrapsEnabled() const {
        static const bool disabled = [] {
            const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
            return value != nullptr && value[0] == '0';
        }();
        return !disabled && current_page_table->fastmem_arena &&
               !Settings::values.use_reactive_flushing.GetValue() &&
               Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce;
    }

    void RasterizerArmReadTraps(u64 vaddr, u64 size) {
        if (vaddr == 0 || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size) ||
            !ReadTrapsEnabled()) {
            return;
        }
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        std::scoped_lock lk{read_trap_mutex};
        u64 run_begin = 0;
        u64 run_pages = 0;
        const auto protect_run = [&] {
            if (run_pages > 0) {
                // Reads that hit a cached read area must not skip the flush of a new trap.
                read_trap_generation.fetch_add(1, std::memory_order_release);
                host_buffer->Protect(run_begin << YUZU_PAGEBITS, run_pages << YUZU_PAGEBITS,
                                     Common::MemoryPermission{});
                run_pages = 0;
            }
        };
        for (u64 page = vaddr >> YUZU_PAGEBITS; page != page_end; ++page) {
            const bool cached = std::as_const(current_page_table->entries)[page].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            armed_read_trap_count.fetch_add(1, std::memory_order_relaxed);
            if (!cached || !armed_read_traps.insert(page).second) {
                armed_read_trap_count.fetch_sub(1, std::memory_order_relaxed);
                protect_run();
                continue;
            }
            if (run_pages == 0) {
                run_begin = page;
            }
            ++run_pages;
        }
        protect_run();
    }

    bool ReadTrapArmed(u64 vaddr, u64 size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        std::scoped_lock lk{read_trap_mutex};
        const auto it = armed_read_traps.lower_bound(vaddr >> YUZU_PAGEBITS);
        return it != armed_read_traps.end() && *it <= ((vaddr + size - 1) >> YUZU_PAGEBITS);
    }

    /// Takes read permission away again from armed pages that were just cached again.
    void ProtectArmedPages(u64 vaddr, u64 size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        for (auto it = armed_read_traps.lower_bound(vaddr >> YUZU_PAGEBITS);
             it != armed_read_traps.end() && *it < page_end; ++it) {
            host_buffer->Protect(*it << YUZU_PAGEBITS, YUZU_PAGESIZE, Common::MemoryPermission{});
        }
    }

    /// A slow-path read of a cached page: its trap, if any, has done its job.
    void DisarmReadTraps(u64 vaddr, size_t size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        auto it = armed_read_traps.lower_bound(vaddr >> YUZU_PAGEBITS);
        while (it != armed_read_traps.end() && *it < page_end) {
            // Still cached (this read came through the rasterizer path): writes stay trapped.
            host_buffer->Protect(*it << YUZU_PAGEBITS, YUZU_PAGESIZE,
                                 Common::MemoryPermission::Read);
            it = armed_read_traps.erase(it);
            armed_read_trap_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /**
     * Maps a region of pages as a specific type.
     *
     * @param page_table The page table to use to perform the mapping.
     * @param base       The base address to begin mapping at.
     * @param size       The total size of the range in bytes.
     * @param target     The target address to begin mapping from.
     * @param type       The page type to map the memory as.
     */
    void MapPages(Common::PageTable& page_table, Common::ProcessAddress base_address, u64 size,
                  Common::PhysicalAddress target, Common::PageType type) {
        auto base = GetInteger(base_address);

        LOG_DEBUG(HW_Memory, "Mapping {:016X} onto {:016X}-{:016X}", GetInteger(target),
                  base * YUZU_PAGESIZE, (base + size) * YUZU_PAGESIZE);

        const auto end = base + size;
        ASSERT_MSG(end <= page_table.entries.size(), "out of range mapping at {:016X}", base + page_table.entries.size());

        if (!target) {
            ASSERT_MSG(type != Common::PageType::Memory,
                       "Mapping memory page without a pointer @ {:016x}", base * YUZU_PAGESIZE);

            while (base != end) {
                page_table.entries[base].ptr.Store(0, type);
                page_table.entries[base].addr = 0;
                page_table.entries[base].block = 0;
                base += 1;
            }
        } else {
            auto orig_base = base;
            while (base != end) {
                auto host_ptr = uintptr_t(system.DeviceMemory().GetPointer<u8>(target)) - (base << YUZU_PAGEBITS);
                auto backing = GetInteger(target) - (base << YUZU_PAGEBITS);
                page_table.entries[base].ptr.Store(host_ptr, type);
                page_table.entries[base].addr = backing;
                page_table.entries[base].block = orig_base << YUZU_PAGEBITS;

                ASSERT_MSG(page_table.entries[base].ptr.Pointer(),
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                target += YUZU_PAGESIZE;
            }
        }
    }

    template<typename F, typename G>
    [[nodiscard]] inline u8* GetPointerImpl(u64 vaddr, F&& on_unmapped, G&& on_rasterizer) const {
        // AARCH64 masks the upper 16 bit of all memory accesses
        vaddr &= 0xffffffffffffULL;
        if (AddressSpaceContains(*current_page_table, vaddr, 1)) [[likely]] {
            // Avoid adding any extra logic to this fast-path block
            const uintptr_t raw_pointer = current_page_table->entries[vaddr >> YUZU_PAGEBITS].ptr.Raw();
            if (const uintptr_t pointer = Common::PageTable::PageInfo::ExtractPointer(raw_pointer)) [[likely]] {
                return reinterpret_cast<u8*>(pointer + vaddr);
            } else {
                switch (Common::PageTable::PageInfo::ExtractType(raw_pointer)) {
                case Common::PageType::Memory:
                    ASSERT_MSG(false, "Mapped memory page without a pointer @ {:#016x}", vaddr);
                    return nullptr;
                case Common::PageType::DebugMemory:
                    return GetPointerFromDebugMemory(vaddr);
                case Common::PageType::RasterizerCachedMemory: {
                    u8* const host_ptr{GetPointerFromRasterizerCachedMemory(vaddr)};
                    on_rasterizer();
                    return host_ptr;
                }
                case Common::PageType::Unmapped: [[unlikely]] {
                    on_unmapped();
                    return nullptr;
                }
                default:
                    UNREACHABLE();
                }
                return nullptr;
            }
        } else {
            on_unmapped();
            return nullptr;
        }
    }

    [[nodiscard]] u8* GetPointer(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr),
            [vaddr]() {
                LOG_ERROR(HW_Memory, "Unmapped GetPointer @ {:#016x}", GetInteger(vaddr));
            },
            []() {});
    }

    [[nodiscard]] u8* GetPointerSilent(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr), []() {}, []() {});
    }

    [[nodiscard]] u8* GetContiguousSpan(VAddr addr, std::size_t size) const noexcept {
        u8* const base = GetPointerSilent(addr);
        if (base == nullptr) {
            return nullptr;
        }

        const VAddr end = addr + size;
        const VAddr first_page = ((addr >> YUZU_PAGEBITS) + 1) << YUZU_PAGEBITS;
        const std::uintptr_t base_ptr = reinterpret_cast<std::uintptr_t>(base);
        for (VAddr page = first_page; page < end; page += YUZU_PAGESIZE) {
            const std::uintptr_t expected = base_ptr + (page - addr);
            if (reinterpret_cast<std::uintptr_t>(GetPointerSilent(page)) != expected) {
                return nullptr;
            }
        }
        return base;
    }

    /// @brief Reads a particular data type out of memory at the given virtual address.
    /// @param vaddr The virtual address to read the data type from.
    /// @tparam T The data type to read out of memory.
    /// @returns The instance of T read from the specified virtual address.
    template <typename T>
    inline T Read(Common::ProcessAddress vaddr) noexcept requires(std::is_trivially_copyable_v<T>) {
        const u64 addr = GetInteger(vaddr);
        if (auto const ptr = GetPointerImpl(addr, [this, addr]() {
            LOG_ERROR(HW_Memory, "Unmapped Read{} @ {:#016x}", sizeof(T) * 8, addr);
            // Diagnostic (fork A): backtrace the first null-page read to find the faulting caller.
            static bool traced = false;
            static bool traced_wild = false;
            if (addr < 0x1000 && !traced) {
                traced = true;
                system.CurrentPhysicalCore().LogBacktrace(system.Kernel());
            } else if (addr >= (1ULL << 48) && !traced_wild) {
                traced_wild = true;
                system.CurrentPhysicalCore().LogBacktrace(system.Kernel());
            }
        }, [&]() {
            HandleRasterizerDownload(addr, sizeof(T));
        }); ptr) [[likely]] {
            // It may be tempting to rewrite this particular section to use "reinterpret_cast";
            // afterall, it's trivially copyable so surely it can be copied ov- Alignment.
            // Remember, alignment. memcpy() will deal with all the alignment extremely fast.
            T result{};
            std::memcpy(&result, ptr, sizeof(T));
            return result;
        }
        return T{};
    }

    /// @brief Writes a particular data type to memory at the given virtual address.
    /// @param vaddr The virtual address to write the data type to.
    /// @tparam T The data type to write to memory.
    template <typename T>
    inline void Write(Common::ProcessAddress vaddr, const T data) noexcept requires(std::is_trivially_copyable_v<T>) {
        auto const addr_c1 = GetInteger(vaddr);
        if (!(sizeof(T) > 1 && (addr_c1 & 4095) + sizeof(T) > 4096)) {
            if (auto const ptr_c1 = GetPointerImpl(addr_c1, [addr_c1] {
                LOG_ERROR(HW_Memory, "Unmapped Read{} @ {:#016X}", sizeof(T) * 8, addr_c1);
            }, [&] {
                HandleRasterizerWrite(addr_c1, sizeof(T));
            }); ptr_c1) {
                std::memcpy(ptr_c1, &data, sizeof(T));
            }
        } else {
            auto const addr_c2 = (addr_c1 & (~0xfff)) + 0x1000;
            // page crossing: say if sizeof(T) = 2, vaddr = 4095
            // 4095 + 2 mod 4096 = 1 => 2 - 1 = 1, thus c1=1, c2=1
            auto const count_c2 = (addr_c1 + sizeof(T)) & 4095;
            auto const count_c1 = sizeof(T) - count_c2;
            if (auto const ptr_c1 = GetPointerImpl(addr_c1, [addr_c1] {
                LOG_ERROR(HW_Memory, "Unmapped Write{} @ {:#016X}", sizeof(T) * 8, addr_c1);
            }, [&] {
                HandleRasterizerWrite(addr_c1, count_c1);
            }); ptr_c1) {
                if (auto const ptr_c2 = GetPointerImpl(addr_c2, [addr_c2] {
                    LOG_ERROR(HW_Memory, "Unmapped Write{} @ {:#016X}", sizeof(T) * 8, addr_c2);
                }, [&] {
                    HandleRasterizerWrite(addr_c2, count_c2);
                }); ptr_c2) {
                    std::array<char, sizeof(T)> tmp = std::bit_cast<std::array<char, sizeof(T)>>(data);
                    std::memcpy(ptr_c1, tmp.data() + 0, count_c1);
                    std::memcpy(ptr_c2, tmp.data() + count_c1, count_c2);
                }
            }
        }
    }

    template <typename T>
    bool WriteExclusive(Common::ProcessAddress vaddr, const T data, const T expected) {
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive{} @ {:#016x} = {:#016x}",
                          sizeof(T) * 8, GetInteger(vaddr), static_cast<u64>(data));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(T)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<T*>(ptr), data, expected);
        }
        return true;
    }

    bool WriteExclusive128(Common::ProcessAddress vaddr, const u128 data, const u128 expected) {
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive128 @ {:#016x} = {:#016x}{:016X}",
                          GetInteger(vaddr), static_cast<u64>(data[1]), static_cast<u64>(data[0]));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(u128)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<u64*>(ptr), data, expected);
        }
        return true;
    }

    /// A rasterizer-cached page whose host pointer is outside the emulated physical memory has no
    /// device address to flush or invalidate (its lookup used to read far out of bounds).
    void ReportForeignPointer(VAddr v_address, const u8* p) const {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1, std::memory_order_relaxed) >= 16) {
            return;
        }
        const auto raw =
            current_page_table->entries[(v_address & 0xffffffffffffULL) >> YUZU_PAGEBITS].Raw();
        LOG_ERROR(HW_Memory,
                  "Rasterizer access at {:#x}: host pointer {} is outside the emulated physical "
                  "memory at {} (page type {}, block {})",
                  v_address, fmt::ptr(p),
                  fmt::ptr(system.DeviceMemory().buffer.BackingBasePointer()),
                  static_cast<u32>(raw.type), static_cast<u32>(raw.block));
    }

    void HandleRasterizerDownload(VAddr v_address, size_t size) {
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        VideoCommon::Nx2Profile::AddShared(VideoCommon::Nx2Profile::SharedCounter::SlowPathReads, 1);
        const size_t core = system.GetCurrentHostThreadID();
        if (core >= Core::Hardware::NUM_CPU_CORES) {
            // A service thread has no slot in the arrays below, and it does not use a slot of a
            // core. It keeps no area, so it asks on each read.
            if (static std::atomic_flag logged; !logged.test_and_set(std::memory_order_relaxed)) {
                LOG_INFO(HW_Memory, "Read of cached guest memory from a thread that is not a core");
            }
            Common::ScratchBuffer<u32> scratch;
            gpu_device_memory->ApplyOpOnPointer(p, scratch, [&](DAddr address) {
                DisarmReadTraps(v_address, size);
                (void)system.GPU().OnCPURead(address, size);
            });
            return;
        }
        auto& current_area = rasterizer_read_areas[core];
        // Both only grow, so the sum changes whenever either does.
        if (const u64 generation = read_trap_generation.load(std::memory_order_acquire) +
                                   gpu_device_memory->ReadAreaGeneration();
            read_area_generations[core] != generation) {
            current_area = {};
            read_area_generations[core] = generation;
        }
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            const DAddr end_address = address + size;
            if (current_area.start_address <= address && end_address <= current_area.end_address)
                [[likely]] {
                return;
            }
            // Flush first: another core's read of the page must fault until the data is there.
            current_area = system.GPU().OnCPURead(address, size);
            if (current_area.release_read_trap) {
                DisarmReadTraps(v_address, size);
            }
        });
    }

    void HandleRasterizerWrite(VAddr v_address, size_t size) {
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        if (!gpu_device_memory->IsPhysicalPointer(p)) [[unlikely]] {
            ReportForeignPointer(v_address, p);
            return;
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            auto& current_area = rasterizer_write_areas[core];
            PAddr subaddress = address >> YUZU_PAGEBITS;
            // Performance note:
            // It may not be a good idea to assume accesses are within the same subaddress (i.e same page)
            // It is often the case the games like to access wildly different addresses. Hence why I propose
            // we should let the compiler just do it's thing...
            if (current_area.last_address != subaddress) {
                // Short circuit the need to check for address/size
                auto const do_collection = (address != 0 && size != 0)
                    && system.GPU().OnCPUWrite(address, size);
                if (do_collection) {
                    current_area.last_address = subaddress;
                } else {
                    return;
                }
            }
            gpu_dirty_managers[core].Collect(address, size);
        });
    }

    struct GPUDirtyState {
        PAddr last_address;
    };

    bool TryInvalidateGPUMemory(const u8* p, size_t size) {
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        bool has_pending_writes = false;
        auto& gpu = system.GPU();
        gpu_device_memory->ApplyOpOnPointer(
            p, scratch_buffers[core], [&](DAddr address) { has_pending_writes |= gpu.MustFlushRegion(address, size); });
        if (has_pending_writes) return false;
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) { gpu.InvalidateRegion(address, size); });
        return true;
    }

    Core::System& system;
    Tegra::MaxwellDeviceMemoryManager* gpu_device_memory{};
    Common::PageTable* current_page_table = nullptr;

    std::array<VideoCore::RasterizerDownloadArea, Core::Hardware::NUM_CPU_CORES> rasterizer_read_areas{};
    std::array<u64, Core::Hardware::NUM_CPU_CORES> read_area_generations{};
    std::atomic<u64> read_trap_generation{};
    std::set<u64> armed_read_traps;
    std::atomic<size_t> armed_read_trap_count{};
    std::mutex read_trap_mutex;
    std::array<GPUDirtyState, Core::Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::array<Common::ScratchBuffer<u32>, Core::Hardware::NUM_CPU_CORES> scratch_buffers{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;
    Common::HostMemory* host_buffer{};
#endif
};

Memory::Memory(Core::System& system_) : system{system_} {
    Reset();
}

Memory::~Memory() = default;

void Memory::Reset() {
    impl = std::make_unique<Impl>(system);
}

void Memory::SetCurrentPageTable(Kernel::KProcess& process) {
    impl->SetCurrentPageTable(process);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                             Common::PhysicalAddress target, Common::MemoryPermission perms,
                             bool separate_heap) {
    impl->MapMemoryRegion(page_table, base, size, target, perms, separate_heap);
}

void Memory::UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         bool separate_heap) {
    impl->UnmapRegion(page_table, base, size, separate_heap);
}

void Memory::ProtectRegion(Common::PageTable& page_table, Common::ProcessAddress vaddr, u64 size,
                           Common::MemoryPermission perms) {
    impl->ProtectRegion(page_table, GetInteger(vaddr), size, perms);
}

bool Memory::IsValidVirtualAddress(const Common::ProcessAddress vaddr) const {
    const auto& page_table = *impl->current_page_table;
    const size_t page = vaddr >> YUZU_PAGEBITS;
    if (page >= page_table.entries.size()) {
        return false;
    }
    const auto [pointer, type] = page_table.entries[page].ptr.PointerType();
    return pointer != 0 || type == Common::PageType::RasterizerCachedMemory ||
           type == Common::PageType::DebugMemory;
}

bool Memory::IsValidVirtualAddressRange(Common::ProcessAddress base, u64 size) const {
    Common::ProcessAddress end = base + size;
    Common::ProcessAddress page = Common::AlignDown(GetInteger(base), YUZU_PAGESIZE);

    for (; page < end; page += YUZU_PAGESIZE) {
        if (!IsValidVirtualAddress(page)) {
            return false;
        }
    }

    return true;
}

u8* Memory::GetPointer(Common::ProcessAddress vaddr) {
    return impl->GetPointer(vaddr);
}

u8* Memory::GetPointerSilent(Common::ProcessAddress vaddr) {
    return impl->GetPointerSilent(vaddr);
}

const u8* Memory::GetPointer(Common::ProcessAddress vaddr) const {
    return impl->GetPointer(vaddr);
}

u8 Memory::Read8(const Common::ProcessAddress addr) {
    return impl->Read<u8>(addr);
}

u16 Memory::Read16(const Common::ProcessAddress addr) {
    return impl->Read<u16_le>(addr);
}

u32 Memory::Read32(const Common::ProcessAddress addr) {
    return impl->Read<u32_le>(addr);
}

bool Memory::Read32Acquire(const Common::ProcessAddress addr, u32& value) {
    return impl->Read32Acquire(addr, value);
}

u64 Memory::Read64(const Common::ProcessAddress addr) {
    return impl->Read<u64_le>(addr);
}

void Memory::Write8(Common::ProcessAddress addr, u8 data) {
    impl->Write<u8>(addr, data);
}

void Memory::Write16(Common::ProcessAddress addr, u16 data) {
    impl->Write<u16_le>(addr, data);
}

void Memory::Write32(Common::ProcessAddress addr, u32 data) {
    impl->Write<u32_le>(addr, data);
}

void Memory::Write64(Common::ProcessAddress addr, u64 data) {
    impl->Write<u64_le>(addr, data);
}

bool Memory::WriteExclusive8(Common::ProcessAddress addr, u8 data, u8 expected) {
    return impl->WriteExclusive8(addr, data, expected);
}

bool Memory::WriteExclusive16(Common::ProcessAddress addr, u16 data, u16 expected) {
    return impl->WriteExclusive16(addr, data, expected);
}

bool Memory::WriteExclusive32(Common::ProcessAddress addr, u32 data, u32 expected) {
    return impl->WriteExclusive32(addr, data, expected);
}

bool Memory::WriteExclusive64(Common::ProcessAddress addr, u64 data, u64 expected) {
    return impl->WriteExclusive64(addr, data, expected);
}

bool Memory::WriteExclusive128(Common::ProcessAddress addr, u128 data, u128 expected) {
    return impl->WriteExclusive128(addr, data, expected);
}

std::string Memory::ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
    return impl->ReadCString(vaddr, max_length);
}

bool Memory::ReadBlock(const Common::ProcessAddress src_addr, void* dest_buffer,
                       const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, false);
}

bool Memory::ReadBlockUnsafe(const Common::ProcessAddress src_addr, void* dest_buffer,
                             const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, true);
}

const u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) const {
    return impl->GetSpan(src_addr, size);
}

u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) {
    return impl->GetSpan(src_addr, size);
}

bool Memory::WriteBlock(const Common::ProcessAddress dest_addr, const void* src_buffer,
                        const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, false);
}

bool Memory::WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void* src_buffer,
                              const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, true);
}

bool Memory::CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                       const std::size_t size) {
    return impl->CopyBlock(dest_addr, src_addr, size);
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->ZeroBlock(dest_addr, size);
}

void Memory::SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers) {
    impl->gpu_dirty_managers = managers;
}

Result Memory::InvalidateDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->InvalidateDataCache(dest_addr, size);
}

Result Memory::StoreDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->StoreDataCache(dest_addr, size);
}

Result Memory::FlushDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->FlushDataCache(dest_addr, size);
}

void Memory::RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached) {
    impl->RasterizerMarkRegionCached(GetInteger(vaddr), size, cached);
}

void Memory::RasterizerArmReadTraps(Common::ProcessAddress vaddr, u64 size) {
    impl->RasterizerArmReadTraps(GetInteger(vaddr), size);
}

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug) {
    impl->MarkRegionDebug(GetInteger(vaddr), size, debug);
}

NCEAccessFaultAction Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size) {
    [[maybe_unused]] bool mapped = true;
    [[maybe_unused]] bool rasterizer = false;

    u8* const ptr = impl->GetPointerImpl(
        GetInteger(vaddr),
        [&] {
            LOG_ERROR(HW_Memory, "Unmapped InvalidateNCE for {} bytes @ {:#x}", size, GetInteger(vaddr));
            mapped = false;
        },
        [&] { rasterizer = true; });
#ifdef __ANDROID__
    if (!rasterizer && mapped) {
        impl->host_buffer->DeferredMapSeparateHeap(GetInteger(vaddr));
    }
#endif

    if (rasterizer) {
        if (!impl->TryInvalidateGPUMemory(ptr, size)) {
            return NCEAccessFaultAction::Instruction;
        }
    }
    return mapped && ptr != nullptr ? NCEAccessFaultAction::Retry : NCEAccessFaultAction::Unmapped;
}

} // namespace Core::Memory
