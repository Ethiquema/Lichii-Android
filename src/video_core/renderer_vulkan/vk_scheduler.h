// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <queue>
#include <source_location>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/dkb_frame_trace.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "video_core/gpu_split.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCommon {
template <typename Trait>
class QueryCacheBase;
}

namespace Vulkan {

class CommandPool;
class Device;
class Framebuffer;
class GraphicsPipeline;
class StateTracker;

struct QueryCacheParams;
struct RenderingAttachments;

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:
    static constexpr u32 DEPTH_ATTACHMENT_BIT = 1u << 8;
    static constexpr u32 STENCIL_ATTACHMENT_BIT = 1u << 9;
    static constexpr u32 ALL_ATTACHMENTS = 0x3FF;

    explicit Scheduler(const Device& device, StateTracker& state_tracker);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    u64 Flush(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {},
              std::source_location location = std::source_location::current());

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {},
                std::source_location location = std::source_location::current());

    /// On the split GPU thread, a fence or a command list copy submits only if the last
    /// submission is older than a window (EDEN_NX2_FENCE_FLUSH_US, default 500 us, 0 submits at
    /// once); otherwise it goes with the next one. The back submits deferred work before it
    /// sleeps and when another thread asks (WaitSubmitted). True when the flush was deferred.
    bool DeferFlush();
    /// Submits deferred work (the split's back, from its idle hook).
    void FlushDeferred();
    /// Threads other than the split's back: waits until tick is submitted, asking the back to
    /// submit deferred work (logged every 2 s while it waits). False only when the back thread
    /// is not running, so nothing will submit it: the caller then submits it itself.
    bool WaitSubmitted(u64 tick);
    /// Whether everything up to tick has been handed to the worker for submission.
    [[nodiscard]] bool IsSubmitted(u64 tick) const noexcept {
        return tick < master_semaphore->CurrentTick();
    }

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    void MarkFrameEnd(u64 frame);
    void TraceGraphicsPipeline(const std::array<u64, 6>& hashes);
    void TraceComputePipeline(u64 hash);
    /// Worker thread, inside a recorded command: timestamps one draw or dispatch of a sampled
    /// submission. A draw passes its vertex and fragment hashes, a dispatch its compute hash.
    template <typename Func>
    void TraceGpuWork(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash,
                      std::array<u32, 3> size, Func&& work) {
        if (!trace_gpu_recording || !trace_work_pool || trace_work_count == TraceMaxWork) {
            work();
            return;
        }
        const u32 query = trace_work_count * 2;
        trace_work_items[trace_work_count++] = {hash, fragment_hash, size};
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool, query);
        work();
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool, query + 1);
    }

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    /// Requests to begin a renderpass.
    void RequestRenderpass(const Framebuffer* framebuffer, u32 touched = ALL_ATTACHMENTS,
                           u32 written = ALL_ATTACHMENTS);

    bool OverrideLoadOps(const Framebuffer* framebuffer, u32 attachments,
                         VkAttachmentLoadOp load_op, const VkClearValue& value);

    void DiscardResolvedAttachments(VkImageView resolve_view);

    bool CanAliasRenderPass(const Framebuffer* framebuffer) const;

    bool IsRenderPassImage(VkImage image) const;

    /// Requests the current execution context to be able to execute operations only allowed outside
    /// of a renderpass.
    void RequestOutsideRenderPassOperationContext();

    void RequestComputeDispatchContext();

    /// Returns true when a render pass is currently active in the scheduler state.
    bool IsRenderPassActive() const {
        return state.framebuffer_id != 0;
    }

    u64 ActiveRenderPassSerial() const noexcept {
        if (state.framebuffer_id != 0) {
            return renderpass_serial;
        }
        return (std::numeric_limits<u64>::max)();
    }

    u64 WaitForIdleSerial() const noexcept {
        return wfi_serial;
    }

    void NotifyWaitForIdle() noexcept {
        ++wfi_serial;
    }

    void MarkRenderPassWrites() noexcept {
        renderpass_writes = true;
    }

    bool HasDepthWrites() const noexcept {
        return (attachments_written & DEPTH_ATTACHMENT_BIT) != 0;
    }

    /// Update the pipeline to the current execution context.
    bool UpdateGraphicsPipeline(GraphicsPipeline* pipeline);

    /// Worker thread only: whether the bound graphics pipeline failed to build, so the draws
    /// recorded after it must be dropped instead of reaching the driver without a pipeline.
    void SetGraphicsPipelineMissing(bool missing) {
        graphics_pipeline_missing = missing;
    }
    [[nodiscard]] bool GraphicsPipelineMissing() const {
        return graphics_pipeline_missing;
    }

    /// Update the rescaling state. Returns true if the state has to be updated.
    bool UpdateRescaling(bool is_rescaling, bool mesh_layout);

    /// Returns true when the descriptor buffer chunk has to be bound into the command buffer.
    bool UpdateDescriptorBufferChunk(u32 descriptor_chunk);

    /// Invalidates current command buffer state except for render passes
    void InvalidateState();

    /// Assigns the query cache.
    void SetQueryCache(VideoCommon::QueryCacheBase<QueryCacheParams>& query_cache_) {
        query_cache = &query_cache_;
    }

    // Registers a callback to perform on queue submission.
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// Send work to a separate thread.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer, vk::CommandBuffer>
    void RecordWithUploadBuffer(T&& command) {
        if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
            VideoCommon::GpuSplit::CheckOwner("Scheduler::Record");
        }
        if (chunk->Record(command)) {
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void Record(T&& c) {
        if (pending_begin) {
            FlushPendingRenderPass();
        }
        this->RecordWithUploadBuffer(
            [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
                command(cmdbuf);
            });
    }

    bool RetractUnrecordedRenderPass();

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    /// Changes whenever recorded command buffer state may have been reset (InvalidateState, the
    /// end of a render pass), so a binding recorded at the same epoch is still bound.
    [[nodiscard]] u64 RecordingEpoch() const noexcept {
        return recording_epoch;
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick) {
        if (tick >= master_semaphore->CurrentTick()) {
            Flush();
        }
        master_semaphore->Wait(tick);
    }

    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() const noexcept {
        return *master_semaphore;
    }

    std::mutex submit_mutex;

private:
    void ApplyFramePacing(double target_fps) {
        if (!Settings::values.use_speed_limit.GetValue() || target_fps <= 0.0) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (last_target_fps != target_fps) {
            frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / target_fps));
            max_frame_count = static_cast<int>(0.1 * target_fps);
            last_target_fps = target_fps;
            frame_counter = 0;
            start_time = now;
        }
        frame_counter++;
        auto target_time = start_time + frame_interval * frame_counter;
        if (target_time >= now) {
            auto sleep_time = target_time - now;
            if (sleep_time > std::chrono::milliseconds(15)) {
                std::this_thread::sleep_for(sleep_time - std::chrono::milliseconds(1));
            }
            while (std::chrono::steady_clock::now() < target_time) {
                std::this_thread::yield();
            }
        } else if (frame_counter > max_frame_count) {
            frame_counter = 0;
            start_time = now;
        }
    }

    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const override {

            command(cmdbuf, upload_cmdbuf);
        }

        T& Get() noexcept {
            return command;
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf);

        template <typename T>
        T* Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return nullptr;
            }
            Command* const current_last = last;
            FuncType* const recorded =
                new (data.data() + command_offset) FuncType(std::move(command));
            last = recorded;

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return &recorded->Get();
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return command_offset == 0;
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data;
    };

    struct State {
        u64 framebuffer_id = 0;
        GraphicsPipeline* graphics_pipeline = nullptr;
        bool rendering = false;
        bool discards_msaa_color = false;
        bool discards_msaa_depth = false;
        u32 num_color = 0;
        bool has_depth = false;
        bool has_stencil = false;
        u32 layer_count = 1;
        bool is_rescaling = false;
        bool rescaling_defined = false;
        bool mesh_layout = false;
        bool needs_state_enable_refresh = false;
        bool uses_transform_feedback = false;
    };

    void BeginRenderPassImpl(const Framebuffer* framebuffer);

    void BindFramebuffer(const Framebuffer* framebuffer);

    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffer();

    void TraceGpuSubmission();
    void ReportWorkTimes();
    void TraceShader(u64 hash, u32 stage);

    u64 SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore);

    void EndPendingOperations();

    void RecordDynamicBegin(const DeferredClear* clear);

    void FlushPendingRenderPass();

    void EndRenderPass();

    void PublishComputeWrites();

    void RelaxAttachmentOps(RenderingAttachments& attachments) const;

    u32 RefineImageAccesses(const RenderingAttachments& attachments,
                            std::array<VkAccessFlags2, 9>& accesses) const;

    void AcquireNewChunk();

    const Device& device;
    StateTracker& state_tracker;

    std::unique_ptr<MasterSemaphore> master_semaphore;
    bool compute_writes_pending{};
    bool graphics_pipeline_missing{};
    std::unique_ptr<CommandPool> command_pool;

    vk::QueryPool trace_query_pool;
    static constexpr u32 TraceMaxSubmissions = 512;
    struct SubmissionTrace {
        struct ShaderUse {
            u64 hash{};
            u32 stage{};
        };
        u64 tick{};
        u32 draw_attempts{};
        u32 dispatch_attempts{};
        u32 num_shaders{};
        u32 untracked_uses{};
        std::array<ShaderUse, 32> shaders{};
    };
    SubmissionTrace trace_work;
    std::vector<SubmissionTrace> trace_submissions;
    /// EDEN_NX2_HANG_WATCH: pipelines recorded into the last submissions, for a stalled wait.
    struct HangSubmission {
        u64 tick{};
        std::vector<std::string> pipelines;
    };
    void HangRecord(std::string pipeline);
    void ReportStall(u64 tick, u64 gpu_tick, u64 next_tick, u64 waited_s);
    std::vector<std::string> hang_work; ///< Worker thread: the submission being recorded
    std::mutex hang_mutex;
    std::deque<HangSubmission> hang_submissions;
    static constexpr u32 TraceMaxWork = 4096;
    struct WorkTrace {
        u64 hash{};
        u64 fragment_hash{};
        /// Dispatch grid, or vertex and instance counts of a draw; zero when indirect.
        std::array<u32, 3> size{};
    };
    vk::QueryPool trace_work_pool;
    std::vector<WorkTrace> trace_work_items;
    u32 trace_work_count{};
    Common::DkbFrameTrace::RateLimit trace_gpu_rate;
    u64 trace_gpu_tick{};
    u64 trace_frame_end{};
    u64 trace_sample_frame{};
    u32 trace_gpu_count{};
    u64 trace_gpu_skipped{};
    u32 trace_timestamp_bits{};
    float trace_timestamp_period{};
    bool trace_gpu_recording{};
    bool trace_gpu_sampling{};
    bool trace_gpu_pending{};
    bool trace_gpu_failed{};

    VideoCommon::QueryCacheBase<QueryCacheParams>* query_cache = nullptr;

    vk::CommandBuffer current_cmdbuf;
    vk::CommandBuffer current_upload_cmdbuf;

    std::unique_ptr<CommandChunk> chunk;
    std::function<void()> on_submit;

    State state;
    u64 recording_epoch = 0;

    u64 renderpass_serial = 0;
    u64 alias_framebuffer_id = 0;
    u64 wfi_serial = 0;
    bool renderpass_writes = false;
    bool renderpass_pristine = false;
    bool compute_writes = false;
    u32 attachments_touched = 0;
    u32 attachments_written = 0;
    RenderingAttachments* recorded_attachments = nullptr;
    RenderingAttachments* ended_attachments = nullptr;
    VkMemoryBarrier2 renderpass_write_barrier{};
    VkMemoryBarrier2 compute_write_barrier{};
    VkMemoryBarrier2 upload_write_barrier{};
    u32 num_renderpass_images = 0;
    std::array<VkImage, 9> renderpass_images{};
    std::array<VkImageSubresourceRange, 9> renderpass_image_ranges{};
    std::array<VkImageView, 9> renderpass_views{};
    VkExtent2D renderpass_extent{};
    u32 renderpass_layers = 0;

    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    bool worker_waiting{}; ///< The worker is waiting for work (guarded by queue_mutex)
    std::jthread worker_thread;
};

} // namespace Vulkan
