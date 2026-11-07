// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/deferred_destruction_queue.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

class FenceBase {
public:
    explicit FenceBase(bool is_stubbed_) : is_stubbed{is_stubbed_} {}

    bool IsStubbed() const {
        return is_stubbed;
    }

protected:
    bool is_stubbed;
};

template <typename Traits>
class FenceManager {
    using TFence = typename Traits::FenceType;
    using TTextureCache = typename Traits::TextureCacheType;
    using TBufferCache = typename Traits::BufferCacheType;
    using TQueryCache = typename Traits::QueryCacheType;
    static constexpr bool can_async_check = Traits::HAS_ASYNC_CHECK;

public:
    /// Notify the fence manager about a new frame
    void TickFrame() {
        std::unique_lock lock(ring_guard);
        ++retire_tick;
        sentenced_fences.Reclaim(retire_tick > RETIRE_DELAY ? retire_tick - RETIRE_DELAY : 0);
    }

    // Unlike other fences, this one doesn't
    void SignalOrdering() {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.AccumulateFlushes();
    }

    void SignalReference() {
        std::function<void()> do_nothing([] {});
        SignalFence(std::move(do_nothing));
    }

    void SyncOperation(std::function<void()>&& func) {
        // Not only the GPU thread queues these (query cache work runs elsewhere too), while
        // SignalFence moves the list into pending_operations: unguarded, the list was corrupted
        // and the fencing thread crashed walking it. Its own lock, held for no callbacks.
        std::scoped_lock lock{uncommitted_guard};
        uncommitted_operations.emplace_back(std::move(func));
    }

    void SignalFence(std::function<void()>&& func, bool ordered = false) {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        const bool should_flush = ShouldFlush();
        const bool delay_fence = ordered || Settings::IsGPULevelHigh() || Settings::getDebugKnobAt(4) ||
                                 (Settings::IsGPULevelMedium() && should_flush);
        CommitAsyncFlushes();
        TFence new_fence = CreateFence(!should_flush && !ordered);
        if constexpr (can_async_check) {
            guard.lock();
        }
        {
            std::scoped_lock lock{uncommitted_guard};
            if (delay_fence) {
                uncommitted_operations.emplace_back(std::move(func));
            }
            pending_operations.emplace_back(std::move(uncommitted_operations));
            uncommitted_operations.clear(); // moved-from: make it empty explicitly
            uncommitted_operations.reserve(64);
        }
        pending_flushes.push_back(committed_flushes);
        QueueFence(new_fence);
        if (!delay_fence) {
            func();
        }
        fences.push(std::move(new_fence));
        if constexpr (can_async_check) {
            guard.unlock();
            cv.notify_all();
        }
        if (delay_fence) {
            SubmitDeferredWork();
        }
        rasterizer.InvalidateGPUCache();
    }

    void SignalSyncPoint(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            syncpoint_manager.IncrementGuest(value);
        }
        std::function<void()> func([this, value, count] {
            for (u32 i = 0; i < count; ++i) {
                syncpoint_manager.IncrementHost(value);
            }
        });
        SignalFence(std::move(func));
    }

    void WaitPendingFences([[maybe_unused]] bool force) {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<true>();
        } else {
            if (!force) {
                return;
            }
            std::mutex wait_mutex;
            std::condition_variable wait_cv;
            std::atomic<bool> wait_finished{};
            std::function<void()> func([&] {
                std::scoped_lock lk(wait_mutex);
                wait_finished.store(true, std::memory_order_relaxed);
                wait_cv.notify_all();
            });
            SignalFence(std::move(func));
            // With delayed fence operations the callback waits for this fence's tick, whose
            // submission may be deferred to the waiting thread itself (the GPU split's back).
            SubmitDeferred();
            std::unique_lock lk(wait_mutex);
            wait_cv.wait(
                lk, [&wait_finished] { return wait_finished.load(std::memory_order_relaxed); });
        }
    }

protected:
    explicit FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                          TTextureCache& texture_cache_, TBufferCache& buffer_cache_,
                          TQueryCache& query_cache_)
        : rasterizer{rasterizer_}, gpu{gpu_}, syncpoint_manager{gpu.Host1x().GetSyncpointManager()},
          texture_cache{texture_cache_}, buffer_cache{buffer_cache_}, query_cache{query_cache_} {
        if constexpr (can_async_check) {
            fence_thread =
                std::jthread([this](std::stop_token token) { ReleaseThreadFunc(token); });
        }
    }

    virtual ~FenceManager() {
        if constexpr (can_async_check) {
            fence_thread.request_stop();
            cv.notify_all();
            fence_thread.join();
        }
    }

    /// Creates a Fence Interface, does not create a backend fence if 'is_stubbed' is
    /// true
    virtual TFence CreateFence(bool is_stubbed) = 0;
    /// Queues a fence into the backend if the fence isn't stubbed.
    virtual void QueueFence(TFence& fence) = 0;
    /// Notifies that the backend fence has been signaled/reached in host GPU.
    virtual bool IsFenceSignaled(TFence& fence) const = 0;
    /// Waits until a fence has been signalled by the host GPU.
    virtual void WaitFence(TFence& fence) = 0;
    /// Submits command work whose submission was deferred (Vulkan: Scheduler::DeferFlush).
    virtual void SubmitDeferred() {}

    VideoCore::RasterizerInterface& rasterizer;
    Tegra::GPU& gpu;
    Tegra::Host1x::SyncpointManager& syncpoint_manager;
    TTextureCache& texture_cache;
    TBufferCache& buffer_cache;
    TQueryCache& query_cache;

private:
    template <bool force_wait>
    void TryReleasePendingFences() {
        while (!fences.empty()) {
            TFence& current_fence = fences.front();
            if (ShouldWait() && !IsFenceSignaled(current_fence)) {
                if constexpr (force_wait) {
                    WaitFence(current_fence);
                } else {
                    return;
                }
            }
            PopAsyncFlushes(pending_flushes.front());
            pending_flushes.pop_front();
            auto operations = std::move(pending_operations.front());
            pending_operations.pop_front();
            for (auto& operation : operations) {
                operation();
            }
            {
                std::unique_lock lock(ring_guard);
                sentenced_fences.Push(std::move(current_fence), retire_tick);
            }
            fences.pop();
        }
    }

    void ReleaseThreadFunc(std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPUFencingThread");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        Common::SetCurrentThreadToPerformanceCores();

        // Every fence the host GPU has already passed is released together: Switch 2 titles
        // signal thousands a second and the host GPU completes them in bursts, so each cache's
        // lock is taken once per burst instead of once per fence (the GPU thread, which holds
        // them for nearly every command, spun, slept and woke this thread for each one).
        // EDEN_NX2_FENCE_BATCH=0: one fence at a time (for comparisons).
        static const size_t MaxBatch = [] {
            const char* value = std::getenv("EDEN_NX2_FENCE_BATCH");
            return value != nullptr && value[0] == '0' ? size_t{1} : size_t{64};
        }();
        struct Released {
            TFence fence;
            OperationList operations;
            u8 flushes;
        };
        std::vector<Released> batch;
        batch.reserve(MaxBatch);
        const auto take_front = [&] {
            batch.push_back({std::move(fences.front()), std::move(pending_operations.front()),
                             pending_flushes.front()});
            fences.pop();
            pending_operations.pop_front();
            pending_flushes.pop_front();
        };
        while (!stop_token.stop_requested()) {
            {
                std::unique_lock lock(guard);
                cv.wait(lock, [&] { return stop_token.stop_requested() || !fences.empty(); });
                if (stop_token.stop_requested()) [[unlikely]] {
                    return;
                }
                take_front();
            }
            if (!batch.front().fence->IsStubbed()) {
                WaitFence(batch.front().fence);
            }
            try {
                PopAsyncFlushes();
            } catch (const std::exception& e) {
                LOG_CRITICAL(Render_Vulkan, "GPUFencingThread: exception in PopAsyncFlushes: {}", e.what());
                throw;
            }
            for (auto& operation : current_operations) {
                operation();
            }
            {
                std::unique_lock lock(ring_guard);
                sentenced_fences.Push(std::move(current_fence), retire_tick);
            }
            batch.clear();
        }
    }

    bool ShouldWait() const {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return texture_cache.ShouldWaitAsyncFlushes() || buffer_cache.ShouldWaitAsyncFlushes() ||
               query_cache.ShouldWaitAsyncFlushes();
    }

    static constexpr u8 TextureFlushes = 1;
    static constexpr u8 BufferFlushes = 2;

    void PopAsyncFlushes() {
        {
            std::scoped_lock lock{texture_cache.mutex};
            texture_cache.PopAsyncFlushes();
        }
        buffer_cache.PopAsyncFlushes();
        query_cache.PopAsyncFlushes();
    }

    struct CacheFlushes {
        bool should_flush;  ///< Downloads to wait for: the fence must reach the host GPU
        u8 committed;       ///< TextureFlushes, BufferFlushes: the caches that committed downloads
    };

    /// Commits the texture and buffer caches' downloads for a new fence (the query cache's are
    /// committed once a fence is made). Switch 2 titles signal a fence per compute dispatch
    /// (thousands a second), mostly with nothing or only buffer data to download: each cache's
    /// lock is taken once, alone, and the fencing thread later takes only the lock of the cache
    /// with work, instead of holding both while it copies (the GPU thread's draws and uploads
    /// waited for it).
    CacheFlushes CommitCacheFlushes() {
        CacheFlushes flushes{};
        {
            std::scoped_lock lock{texture_cache.mutex};
            flushes.should_flush = texture_cache.HasUncommittedFlushes();
            if (texture_cache.HasAsyncFlushWork()) {
                texture_cache.CommitAsyncFlushes();
                flushes.committed |= TextureFlushes;
            }
        }
        {
            std::scoped_lock lock{buffer_cache.mutex};
            if (buffer_cache.HasUncommittedFlushes()) {
                buffer_cache.CommitAsyncFlushes();
                flushes.should_flush = true;
                flushes.committed |= BufferFlushes;
            }
        }
        return flushes;
    }

    /// Whether a fence with no downloads would do nothing, so need not be made: with fast GPU
    /// fences its callback runs at once anyway, and most of the per-dispatch fences of Switch 2
    /// titles have no queries or queued operations either. Making, queueing and retiring them
    /// cost the GPU thread ~5% and woke the fencing thread for each. One in 64 is still made, so
    /// the operations fences carry along (query slot cleanup) keep running.
    bool SkipEmptyFence() {
        if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            if (skipped_fences >= 64 || query_cache.HasPendingFenceWork()) {
                return false;
            }
            {
                std::scoped_lock lock{uncommitted_guard};
                if (!uncommitted_operations.empty()) {
                    return false;
                }
            }
            ++skipped_fences;
            return true;
        } else {
            return false;
        }
    }

    std::queue<TFence> fences;
    // LOCAL TEST (Zelda): vectors, not deques. MSVC's deque of std::function holds one element per
    // block, so every queued operation (one per query report, ~1000 objects a frame in Ocarina's
    // Kokiri Forest) allocated a block.
    using OperationList = std::vector<std::function<void()>>;
    OperationList uncommitted_operations;
    std::deque<OperationList> pending_operations;
    std::deque<u8> pending_flushes; ///< Per fence: the caches that committed downloads
    u32 skipped_fences{}; ///< Empty fences skipped since the last one made (GPU thread only)

    std::mutex guard;
    std::mutex ring_guard;
    std::mutex uncommitted_guard; ///< Guards uncommitted_operations
    std::condition_variable cv;

    std::jthread fence_thread;

    static constexpr u64 RETIRE_DELAY = 8;
    u64 retire_tick = 1;
    DeferredDestructionQueue<TFence> sentenced_fences;
};

} // namespace VideoCommon
