// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Staging buffers no request took for this many frames are freed once the GPU is done with them.
constexpr u64 STAGING_KEEP_FRAMES = 60;

// At most this many staging buffers are freed per frame, so a burst does not stall one frame.
constexpr size_t STAGING_DELETIONS_PER_FRAME = 32;

// A request may take a free buffer up to this many fine size classes larger than its own
// (at most 1.5 times its size), instead of allocating a new one.
constexpr u32 STAGING_REUSE_LARGER_CLASSES = 2;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;

size_t GetStreamBufferSize(const Device& device) {
    if (!device.HasDebuggingToolAttached()) {
        return max_stream_buffer_size;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = std::max<size_t>(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= max_stream_buffer_size) {
            size = size * 40 / 100;
        }
    } else {
        size = max_stream_buffer_size;
    }
    return std::min<size_t>(Common::AlignUp(size, max_alignment), max_stream_buffer_size);
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator_, Scheduler& scheduler_)
    : memory_allocator{memory_allocator_}, scheduler{scheduler_}
    , stream_buffer_size{GetStreamBufferSize(device, 256_MiB, MAX_ALIGNMENT)}
{
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    // Some drivers are more sensitive to increased buffer sizes
    try {
        stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    } catch (vk::Exception& e) {
        LOG_ERROR(Render_Vulkan, "Can't fit {} bytes buffer, halving", stream_ci.size);
        stream_buffer_size = GetStreamBufferSize(device, 128_MiB, MAX_ALIGNMENT);
        stream_ci.size = stream_buffer_size;
        stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    }
    region_size = stream_buffer_size / StagingBufferPool::NUM_SYNCS;
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(const Device& device, size_t size, MemoryUsage usage, bool deferred) {
    return (!deferred && usage == MemoryUsage::Upload && size <= region_size)
        ? GetStreamBuffer(device, size)
        : GetStagingBuffer(device, size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    auto& entries = GetCache(ref.usage)[ref.size_class].entries;
    const auto is_this_one = [&ref](const StagingBuffer& entry) {
        return entry.index == ref.index;
    };
    auto it = std::find_if(entries.begin(), entries.end(), is_this_one);
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = scheduler.CurrentTick();
    it->frame = frame;
    it->deferred = false;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    // Every size class is checked each frame: buffers requests keep taking stay, idle ones go.
    size_t deletions_left = STAGING_DELETIONS_PER_FRAME;
    ReleaseCache(upload_cache, deletions_left);
    ReleaseCache(download_cache, deletions_left);
    ReleaseCache(device_local_cache, deletions_left);
}

// Sizes up to 1 MiB round up to a power of two. Larger ones round up to a quarter step of the
// power of two below them (1.25, 1.5, 1.75 or 2 times it), so a 17 MiB upload takes a 20 MiB
// buffer instead of a 32 MiB one.
u32 StagingBufferPool::SizeClass(size_t size) {
    const u32 log2 = static_cast<u32>(Common::Log2Ceil(size));
    if (log2 <= FINE_SIZE_CLASSES_FROM_LOG2) {
        return log2;
    }
    const u32 base_log2 = log2 - 1;
    const size_t step = size_t{1} << (base_log2 - 2);
    const size_t steps = Common::DivCeil(size - (size_t{1} << base_log2), step);
    return FINE_SIZE_CLASSES_FROM_LOG2 + (base_log2 - FINE_SIZE_CLASSES_FROM_LOG2) * 4 +
           static_cast<u32>(steps);
}

size_t StagingBufferPool::ClassSize(u32 size_class) {
    if (size_class <= FINE_SIZE_CLASSES_FROM_LOG2) {
        return size_t{1} << size_class;
    }
    const u32 fine = size_class - FINE_SIZE_CLASSES_FROM_LOG2 - 1;
    const u32 base_log2 = FINE_SIZE_CLASSES_FROM_LOG2 + fine / 4;
    return (size_t{1} << base_log2) + (fine % 4 + 1) * (size_t{1} << (base_log2 - 2));
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    const size_t wanted_begin = Region(free_iterator) + 1;
    const size_t wanted_end = (std::min)(Region(iterator + size) + 1, NUM_SYNCS);
    if (AreRegionsActive(wanted_begin, wanted_end)) {
        if (auto ref = OverflowStreamBuffer(size, wanted_begin, wanted_end)) {
            return *ref;
        }
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = (std::max)(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS, current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        if (AreRegionsActive(0, Region(size) + 1)) {
            if (auto ref = OverflowStreamBuffer(size, 0, Region(size) + 1)) {
                return *ref;
            }
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = VkDeviceSize(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .size_class{},
        .index{},
    };
}

u64 StagingBufferPool::MaxRegionTick(size_t region_begin, size_t region_end) const {
    u64 tick = 0;
    for (size_t region = region_begin; region < region_end; ++region) {
        tick = (std::max)(tick, sync_ticks[region]);
    }
    return tick;
}

std::optional<StagingBufferRef> StagingBufferPool::OverflowStreamBuffer(size_t size,
                                                                        size_t region_begin,
                                                                        size_t region_end) {
    if (cache_bytes[static_cast<size_t>(MemoryUsage::Upload)] < stream_buffer_size) {
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    if (auto ref = TryGetReservedBuffer(size, MemoryUsage::Upload, false)) {
        return ref;
    }
    scheduler.Wait(MaxRegionTick(region_begin, region_end));
    return std::nullopt;
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end, [gpu_tick](u64 sync_tick) {
        return gpu_tick < sync_tick;
    });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    const u32 size_class = SizeClass(size);
    const u32 last_class =
        size_class > FINE_SIZE_CLASSES_FROM_LOG2
            ? (std::min)(size_class + STAGING_REUSE_LARGER_CLASSES, static_cast<u32>(NUM_LEVELS - 1))
            : size_class;
    for (u32 reuse_class = size_class; reuse_class <= last_class; ++reuse_class) {
        if (const std::optional<StagingBufferRef> ref =
                TryGetReservedBuffer(reuse_class, usage, deferred)) {
            return *ref;
        }
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(u32 size_class,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[size_class];

    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    cache_level.iterate_index = std::distance(entries.begin(), it) + 1;
    it->tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick();
    it->frame = frame;
    ASSERT(!it->deferred);
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred) {
    const u32 size_class = SizeClass(size);
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = ClassSize(size_class),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer& entry = GetCache(usage)[size_class].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .size_class = size_class,
        .index = unique_ids++,
        .tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick(),
        .frame = frame,
        .deferred = deferred,
    });
    return entry.Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

void StagingBufferPool::ReleaseCache(StagingBuffersCache& cache, size_t& deletions_left) {
    const auto is_deletable = [this, &deletions_left](const StagingBuffer& entry) {
        if (deletions_left == 0 || frame - entry.frame <= STAGING_KEEP_FRAMES ||
            !scheduler.IsFree(entry.tick)) {
            return false;
        }
        --deletions_left;
        return true;
    };
    // Largest first: they hold the most memory.
    for (auto level = cache.rbegin(); level != cache.rend(); ++level) {
        if (deletions_left == 0) {
            return;
        }
        StagingBuffers& staging = *level;
        auto& entries = staging.entries;
        if (entries.empty()) {
            continue;
        }
        std::erase_if(entries, is_deletable);
        if (staging.iterate_index > entries.size()) {
            staging.iterate_index = 0;
        }
    }
}

u64 StagingBufferPool::ReclaimMemory(u64 target_bytes) {
    u64 freed = 0;
    const auto is_deletable = [this](const StagingBuffer& entry) {
        return scheduler.IsFree(entry.tick);
    };
    const auto reclaim_cache = [&](StagingBuffersCache& cache) {
        for (size_t level = NUM_LEVELS; level-- > 0 && freed < target_bytes;) {
            auto& staging = cache[level];
            auto& entries = staging.entries;
            if (entries.empty()) {
                continue;
            }
            const u64 entry_bytes = 1ULL << level;
            auto it = entries.begin();
            while (it != entries.end() && freed < target_bytes) {
                if (is_deletable(*it)) {
                    it = entries.erase(it);
                    freed += entry_bytes;
                } else {
                    ++it;
                }
            }
            staging.delete_index = 0;
            staging.iterate_index = 0;
        }
    };
    reclaim_cache(device_local_cache);
    reclaim_cache(upload_cache);
    reclaim_cache(download_cache);
    return freed;
}

} // namespace Vulkan
