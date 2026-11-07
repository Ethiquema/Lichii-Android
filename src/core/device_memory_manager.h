// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "common/range_mutex.h"
#include "common/scratch_buffer.h"
#include "common/virtual_buffer.h"

namespace Common {
class HostMemory;
}

namespace Core {

constexpr size_t DEVICE_PAGEBITS = 12ULL;
constexpr size_t DEVICE_PAGESIZE = 1ULL << DEVICE_PAGEBITS;
constexpr size_t DEVICE_PAGEMASK = DEVICE_PAGESIZE - 1ULL;

class DeviceMemory;

namespace Memory {
class Memory;
}

template <typename DTraits>
struct DeviceMemoryManagerAllocator;

struct Asid {
    size_t id;
};

template <typename Traits>
class DeviceMemoryManager {
    using DeviceInterface = typename Traits::DeviceInterface;
    using DeviceMethods = typename Traits::DeviceMethods;

public:
    class MirrorMapping {
    public:
        MirrorMapping() = default;
        MirrorMapping(u8* mapped_base_, size_t mapped_size_, size_t data_offset_)
            : mapped_base{mapped_base_}, mapped_size{mapped_size_}, data_offset{data_offset_} {}

        MirrorMapping(const MirrorMapping&) = delete;
        MirrorMapping& operator=(const MirrorMapping&) = delete;

        MirrorMapping(MirrorMapping&& other) noexcept {
            MoveFrom(other);
        }

        MirrorMapping& operator=(MirrorMapping&& other) noexcept {
            if (this != &other) {
                Release();
                MoveFrom(other);
            }
            return *this;
        }

        ~MirrorMapping() {
            Release();
        }

        [[nodiscard]] bool IsValid() const noexcept {
            return mapped_base != nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return IsValid();
        }

        [[nodiscard]] u8* Data() noexcept {
            return mapped_base ? mapped_base + data_offset : nullptr;
        }

        [[nodiscard]] const u8* Data() const noexcept {
            return mapped_base ? mapped_base + data_offset : nullptr;
        }

        [[nodiscard]] size_t Size() const noexcept {
            return mapped_size >= data_offset ? mapped_size - data_offset : 0;
        }

    private:
        void MoveFrom(MirrorMapping& other) noexcept {
            mapped_base = std::exchange(other.mapped_base, nullptr);
            mapped_size = std::exchange(other.mapped_size, 0);
            data_offset = std::exchange(other.data_offset, 0);
        }

        void Release() noexcept {
#if defined(__linux__)
            if (mapped_base) {
                munmap(mapped_base, mapped_size);
            }
#endif
            mapped_base = nullptr;
            mapped_size = 0;
            data_offset = 0;
        }

        u8* mapped_base{};
        size_t mapped_size{};
        size_t data_offset{};
    };

    DeviceMemoryManager(const DeviceMemory& device_memory);
    ~DeviceMemoryManager();

    static constexpr bool HAS_FLUSH_INVALIDATION = true;
    static constexpr size_t AS_BITS = Traits::device_virtual_bits;

    void BindInterface(DeviceInterface* device_inter);

    /// Marks the memory behind [address, address + size) cold (Common::HostMemory::HintCold),
    /// from a low-priority thread: data the host GPU has its own copy of, e.g. uploaded textures.
    void HintCold(DAddr address, size_t size);

    DAddr Allocate(size_t size);
    void AllocateFixed(DAddr start, size_t size);
    void Free(DAddr start, size_t size);

    void Map(DAddr address, VAddr virtual_address, size_t size, Asid asid, bool track = false);

    void Unmap(DAddr address, size_t size);

    void TrackContinuityImpl(DAddr address, VAddr virtual_address, size_t size, Asid asid);
    void TrackContinuity(DAddr address, VAddr virtual_address, size_t size, Asid asid) {
        std::scoped_lock lk(mapping_guard);
        TrackContinuityImpl(address, virtual_address, size, asid);
    }

    // Write / Read
    template <typename T>
    T* GetPointer(DAddr address);

    template <typename T>
    const T* GetPointer(DAddr address) const;

    template <typename Func>
    void ApplyOpOnPAddr(PAddr address, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        DAddr subbits = DAddr(address & page_mask);
        const u32 base = compressed_device_addr[(address >> page_bits)];
        if ((base >> MULTI_FLAG_BITS) == 0) [[likely]] {
            const DAddr d_address = (DAddr(base) << page_bits) + subbits;
            operation(d_address);
            return;
        }
        InnerGatherDeviceAddresses(buffer, address);
        for (u32 value : buffer) {
            operation((DAddr(value) << page_bits) + subbits);
        }
    }

    /// Whether p points into the emulated physical memory, the only memory with device addresses.
    bool IsPhysicalPointer(const u8* p) const {
        return (GetRawPhysicalAddr(p) >> page_bits) < compressed_device_addr.size();
    }

    template <typename Func>
    void ApplyOpOnPointer(const u8* p, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        PAddr address = GetRawPhysicalAddr<u8>(p);
        ApplyOpOnPAddr(address, buffer, operation);
    }

    u8* GetPhysicalBase() noexcept {
        return reinterpret_cast<u8*>(physical_base);
    }

    const u8* GetPhysicalBase() const noexcept {
        return reinterpret_cast<const u8*>(physical_base);
    }

    size_t GetPhysicalSize() const noexcept {
        return physical_size;
    }

    std::span<AHardwareBuffer* const> GetBackingHardwareBuffers() const noexcept {
        return ahb_windows;
    }

    size_t GetBackingHardwareBufferWindowSize() const noexcept {
        return ahb_window_size;
    }

    size_t GetBackingHardwareBufferBase() const noexcept {
        return ahb_base;
    }

    bool IsBackingShared() const noexcept {
        return backing_is_shared;
    }

    PAddr GetPhysicalRawAddressFromDAddr(DAddr address) const {
        PAddr subbits = PAddr(address & page_mask);
        auto paddr = tracked_entries[(address >> page_bits)].compressed_physical_ptr;
        if (paddr == 0) {
            return 0;
        }
        return (PAddr(paddr - 1) << page_bits) + subbits;
    }

    template <typename T>
    void Write(DAddr address, T value);

    template <typename T>
    T Read(DAddr address) const;

    u8* GetSpan(const DAddr src_addr, const std::size_t size);
    const u8* GetSpan(const DAddr src_addr, const std::size_t size) const;

    void ReadBlock(DAddr address, void* dest_pointer, size_t size);
    void ReadBlockUnsafe(DAddr address, void* dest_pointer, size_t size);
    void WriteBlock(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockUnsafe(DAddr address, const void* src_pointer, size_t size);

    [[nodiscard]] MirrorMapping CreateMirrorMapping(DAddr address, size_t size) const;
    [[nodiscard]] u64 GetMappingVersion() const noexcept {
        return mapping_version.load(std::memory_order_acquire);
    }

    Asid RegisterProcess(Memory::Memory* memory);
    void UnregisterProcess(Asid id);

    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta);

    // New batch API to update multiple ranges with a single lock acquisition.
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta);

    const Common::HostMemory& GetHostMemory() const noexcept {
        return host_memory;
    }

    /// Makes guest threads drop their cached read areas, after a GPU write to one of them.
    void InvalidateReadAreas() {
        read_area_generation.fetch_add(1, std::memory_order_release);
    }

    u64 ReadAreaGeneration() const {
        return read_area_generation.load(std::memory_order_acquire);
    }

private:
    struct TranslationEntry {
        DAddr guest_page{};
        u8* host_ptr{};
    };

    /// A thread's last four page translations of one manager. Several threads read through
    /// them (the GPU thread, the fencing thread, service threads): one cache shared by all was
    /// cleared by Map on a service thread between the GPU thread's null check of an entry and
    /// its use, and the GPU thread copied from address 0 (title 1 crashed every few minutes).
    struct TranslationCache {
        const void* owner{};
        u64 generation{};
        std::array<TranslationEntry, 4> slots{};
        u32 cursor{};
    };
    static inline thread_local TranslationCache t_cache{};

    /// This thread's translations, emptied when Map or Unmap ran since they were made.
    TranslationCache& Translations() {
        const u64 current = translation_generation.load(std::memory_order_acquire);
        if (t_cache.owner != this || t_cache.generation != current) {
            t_cache = TranslationCache{.owner = this, .generation = current};
        }
        return t_cache;
    }

    // Internal helper that performs the update assuming the caller already holds the necessary lock.
    void UpdatePagesCachedCountNoLock(DAddr addr, size_t size, s32 delta);

private:
    static constexpr size_t device_virtual_bits = Traits::device_virtual_bits;
    static constexpr size_t device_as_size = 1ULL << device_virtual_bits;
    static constexpr size_t physical_min_bits = 32;
    static constexpr size_t physical_max_bits = 35; // covers the 12 GiB layout plus nx2 graphics server memory
    static constexpr size_t page_bits = 12;
    static constexpr size_t page_size = 1ULL << page_bits;
    static constexpr size_t page_mask = page_size - 1ULL;
    static constexpr u32 physical_address_base = 1U << page_bits;
    static constexpr u32 MULTI_FLAG_BITS = 31;
    static constexpr u32 MULTI_FLAG = 1U << MULTI_FLAG_BITS;
    static constexpr u32 MULTI_MASK = ~MULTI_FLAG;

    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) - physical_base);
    }

    void WalkBlock(const DAddr addr, const std::size_t size, auto on_unmapped, auto on_memory,
                   auto increment);

    void CutContinuityAt(size_t page);

    void InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& buffer, PAddr address);

    std::unique_ptr<DeviceMemoryManagerAllocator<Traits>> impl;

    const uintptr_t physical_base;
    const Common::HostMemory& host_memory;
    DeviceInterface* device_inter;
    std::unique_ptr<Common::ThreadWorker> cold_hint_worker;

    struct TrackedEntry {
        VAddr cpu_backing_address;
        u32 continuity_tracker;
        u32 compressed_physical_ptr;
    };
    Common::VirtualBuffer<u32> compressed_device_addr;
    Common::VirtualBuffer<TrackedEntry> tracked_entries;

    // Process memory interfaces

    std::deque<size_t> id_pool;
    std::deque<Memory::Memory*> registered_processes;
    std::atomic<u64> read_area_generation{};

    // Memory protection management

    static constexpr size_t guest_max_as_bits = 39;
    static constexpr size_t guest_as_size = 1ULL << guest_max_as_bits;
    static constexpr size_t guest_mask = guest_as_size - 1ULL;
    static constexpr size_t asid_start_bit = guest_max_as_bits;

    std::pair<Asid, VAddr> ExtractCPUBacking(size_t page_index) {
        auto content = tracked_entries[page_index].cpu_backing_address;
        const VAddr address = content & guest_mask;
        const Asid asid{static_cast<size_t>(content >> asid_start_bit)};
        return std::make_pair(asid, address);
    }

    void InsertCPUBacking(size_t page_index, VAddr address, Asid asid) {
        tracked_entries[page_index].cpu_backing_address = address | (asid.id << asid_start_bit);
    }

    std::atomic<u64> translation_generation{1}; ///< Bumped by Map and Unmap
    using CounterType = u8;
    using CounterAtomicType = std::atomic_uint8_t;
    static constexpr size_t subentries = 8 / sizeof(CounterType);
    static constexpr size_t subentries_mask = subentries - 1;
    static constexpr size_t subentries_shift =
        std::countr_zero(sizeof(u64)) - std::countr_zero(sizeof(CounterType));
    class CounterEntry final {
    public:
        CounterEntry() = default;

        CounterAtomicType& Count(std::size_t page) {
            return values[page & subentries_mask];
        }

        const CounterAtomicType& Count(std::size_t page) const {
            return values[page & subentries_mask];
        }

    private:
        std::array<CounterAtomicType, subentries> values{};
    };
    static_assert(sizeof(CounterEntry) == subentries * sizeof(CounterType),
                  "CounterEntry should be 8 bytes!");

    static constexpr size_t num_counter_entries =
        (1ULL << (device_virtual_bits - page_bits)) / subentries;
    using CachedPages = std::array<CounterEntry, num_counter_entries>;
    std::unique_ptr<CachedPages> cached_pages;
    std::unique_ptr<CachedPages> texture_cached_pages;
    Common::RangeMutex counter_guard;
    std::mutex mapping_guard;
    std::atomic<u64> mapping_version{1};


};

} // namespace Core
