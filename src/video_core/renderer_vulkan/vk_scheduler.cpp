// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/make_unique_for_overwrite.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {
constexpr size_t DEPTH_VIEW = 8;

struct BeginRenderingCommand {
    void operator()(vk::CommandBuffer cmdbuf, vk::CommandBuffer) const {
        BeginRendering(cmdbuf, attachments);
    }

    RenderingAttachments attachments;
};

VkImageView DepthStencilView(const RenderingAttachments& attachments) {
    if (attachments.depth.imageView != VK_NULL_HANDLE) {
        return attachments.depth.imageView;
    }
    return attachments.stencil.imageView;
}

VkAccessFlags2 AttachmentWriteAccess(VkImageAspectFlags aspect_mask) {
    if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}
} // Anonymous namespace

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {
    compute_write_barrier = VkMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = device.GetBufferConsumerStages() | vk::PIPELINE_STAGE_ATTACHMENTS,
        .dstAccessMask = device.GetBufferConsumerAccess() | vk::ACCESS_ATTACHMENTS,
    };
    renderpass_write_barrier = compute_write_barrier;
    renderpass_write_barrier.srcStageMask = vk::PIPELINE_STAGE_GRAPHICS_SHADERS;
    upload_write_barrier = compute_write_barrier;
    upload_write_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    upload_write_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        renderpass_write_barrier.srcStageMask |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        renderpass_write_barrier.srcAccessMask |=
            VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT |
            VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
    }

    vk::SetDeletionTimeline(master_semaphore->CurrentTick());
    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (Settings::getDebugKnobAt(3) && chunk->Empty() && !signal_semaphore && !wait_semaphore) {//xbzk: improves fps by skipping flushes
        const u64 current = CurrentTick();
        return current > 0 ? current - 1 : 0;
    }
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    return SubmitExecution(signal_semaphore, wait_semaphore);
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        bool wake;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            wake = worker_waiting;
        }
        if (wake) {
            event_cv.notify_all();
        }
        event_cv.notify_all();
        recorded_attachments = nullptr;
        ended_attachments = nullptr;
        AcquireNewChunk();
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer) {
    const RenderingAttachments& attachments = framebuffer->Attachments();
    PublishComputeWrites();
    state.framebuffer_id = framebuffer->Id();
    ++renderpass_serial;
    renderpass_pristine = true;
    alias_framebuffer_id = 0;
    std::ranges::transform(attachments.colors, renderpass_views.begin(),
                           &VkRenderingAttachmentInfo::imageView);
    renderpass_views[DEPTH_VIEW] = DepthStencilView(attachments);
    renderpass_extent = attachments.render_area.extent;
    renderpass_layers = attachments.layers;
    attachments_touched = 0;
    attachments_written = 0;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const VkExtent2D render_area = attachments.render_area.extent;
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    BeginRenderingCommand command{attachments};
    BeginRenderingCommand* recorded = chunk->Record(command);
    if (recorded == nullptr) {
        DispatchWork();
        recorded = chunk->Record(command);
    }
    recorded_attachments = &recorded->attachments;
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::BindFramebuffer(const Framebuffer* framebuffer) {
    if (framebuffer->Id() == state.framebuffer_id) {
        return;
    }
    if (CanAliasRenderPass(framebuffer)) {
        alias_framebuffer_id = framebuffer->Id();
        return;
    }
    EndRenderPass();
    BeginRenderPassImpl(framebuffer);
}

bool Scheduler::CanAliasRenderPass(const Framebuffer* framebuffer) const {
    const u64 id = framebuffer->Id();
    if (id == alias_framebuffer_id) {
        return true;
    }
    if (id == state.framebuffer_id || state.framebuffer_id == 0 ||
        !device.IsExtDynamicRenderingUnusedAttachmentsSupported()) {
        return false;
    }
    const RenderingAttachments& attachments = framebuffer->Attachments();
    if (attachments.layers != renderpass_layers ||
        attachments.render_area.extent.width != renderpass_extent.width ||
        attachments.render_area.extent.height != renderpass_extent.height) {
        return false;
    }
    for (u32 slot = 0; slot < attachments.num_colors; ++slot) {
        if (attachments.colors[slot].imageView != renderpass_views[slot]) {
            return false;
        }
    }
    const VkImageView depth_stencil = DepthStencilView(attachments);
    return depth_stencil == VK_NULL_HANDLE || depth_stencil == renderpass_views[DEPTH_VIEW];
}

bool Scheduler::IsRenderPassImage(VkImage image) const {
    const auto images = std::span(renderpass_images).first(num_renderpass_images);
    return std::ranges::find(images, image) != images.end();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer, u32 touched, u32 written) {
    BindFramebuffer(framebuffer);
    renderpass_pristine = false;
    attachments_touched |= touched;
    attachments_written |= written;
}

bool Scheduler::OverrideLoadOps(const Framebuffer* framebuffer, u32 attachments,
                                VkAttachmentLoadOp load_op, const VkClearValue& value) {
    BindFramebuffer(framebuffer);
    if (!renderpass_pristine || recorded_attachments == nullptr) {
        return false;
    }
    const auto set_load_op = [&](VkRenderingAttachmentInfo& attachment) {
        attachment.loadOp = load_op;
        attachment.clearValue = value;
    };
    for (u32 slot = 0; slot < recorded_attachments->num_colors; ++slot) {
        if ((attachments & (1u << slot)) != 0) {
            set_load_op(recorded_attachments->colors[slot]);
        }
    }
    if ((attachments & DEPTH_ATTACHMENT_BIT) != 0) {
        set_load_op(recorded_attachments->depth);
    }
    if ((attachments & STENCIL_ATTACHMENT_BIT) != 0) {
        set_load_op(recorded_attachments->stencil);
    }
    attachments_touched |= attachments;
    attachments_written |= attachments;
    return true;
}

void Scheduler::DiscardResolvedAttachments(VkImageView resolve_view) {
    if (ended_attachments == nullptr) {
        return;
    }
    const auto discard = [resolve_view](VkRenderingAttachmentInfo& attachment) {
        if (attachment.resolveImageView == resolve_view) {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
    };
    std::ranges::for_each(std::span(ended_attachments->colors).first(ended_attachments->num_colors),
                          discard);
    discard(ended_attachments->depth);
    discard(ended_attachments->stencil);
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    EndRenderPass();
    PublishComputeWrites();
}

void Scheduler::RequestComputeDispatchContext() {
    EndRenderPass();
    compute_writes = true;
}

void Scheduler::RelaxAttachmentOps(RenderingAttachments& attachments) const {
    const bool load_op_none = device.IsLoadOpNoneSupported();
    const bool drop_unused = device.IsExtDynamicRenderingUnusedAttachmentsSupported();
    const auto relax = [&](VkRenderingAttachmentInfo& attachment, u32 bit) {
        if (attachment.imageView == VK_NULL_HANDLE) {
            return true;
        }
        if (attachment.resolveMode != VK_RESOLVE_MODE_NONE ||
            attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
            attachment.storeOp != VK_ATTACHMENT_STORE_OP_STORE ||
            (attachments_written & bit) != 0) {
            return false;
        }
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        return (attachments_touched & bit) == 0;
    };
    for (u32 slot = 0; slot < attachments.num_colors; ++slot) {
        VkRenderingAttachmentInfo& color = attachments.colors[slot];
        if (!relax(color, 1u << slot)) {
            continue;
        }
        if (drop_unused) {
            color.imageView = VK_NULL_HANDLE;
        } else if (load_op_none) {
            color.loadOp = VK_ATTACHMENT_LOAD_OP_NONE;
        }
    }
    const bool depth_unused = relax(attachments.depth, DEPTH_ATTACHMENT_BIT);
    const bool stencil_unused = relax(attachments.stencil, STENCIL_ATTACHMENT_BIT);
    if (drop_unused && depth_unused && stencil_unused) {
        attachments.depth.imageView = VK_NULL_HANDLE;
        attachments.stencil.imageView = VK_NULL_HANDLE;
    }
}

u32 Scheduler::RefineImageAccesses(const RenderingAttachments& attachments,
                                   std::array<VkAccessFlags2, 9>& accesses) const {
    const auto stores = [](const VkRenderingAttachmentInfo& attachment) {
        return attachment.imageView != VK_NULL_HANDLE &&
               attachment.storeOp != VK_ATTACHMENT_STORE_OP_NONE;
    };
    u32 unused_images = 0;
    u32 image = 0;
    const auto refine = [&](bool used, bool stored) {
        if (!used) {
            unused_images |= 1u << image;
        }
        if (!stored) {
            accesses[image] = 0;
        }
        ++image;
    };
    for (u32 slot = 0; slot < attachments.num_colors; ++slot) {
        if (renderpass_views[slot] != VK_NULL_HANDLE) {
            const VkRenderingAttachmentInfo& color = attachments.colors[slot];
            refine(color.imageView != VK_NULL_HANDLE, stores(color));
        }
    }
    if (renderpass_views[DEPTH_VIEW] != VK_NULL_HANDLE) {
        refine(DepthStencilView(attachments) != VK_NULL_HANDLE,
               stores(attachments.depth) || stores(attachments.stencil));
    }
    return unused_images;
}

void Scheduler::PublishComputeWrites() {
    if (!std::exchange(compute_writes, false)) {
        return;
    }
    Record([barrier = &compute_write_barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(*barrier);
    });
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] {
                worker_waiting = !TryPopQueue(work);
                return !worker_waiting;
            });
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            try {
                work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);
            } catch (const vk::Exception& e) {
                LOG_CRITICAL(Render_Vulkan, "VulkanWorker: vk::Exception in ExecuteAll: result={}",
                             static_cast<int>(e.GetResult()));
                throw;
            } catch (const std::exception& e) {
                LOG_CRITICAL(Render_Vulkan, "VulkanWorker: exception in ExecuteAll: {}", e.what());
                throw;
            }

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    master_semaphore->Refresh();
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    vk::SetDeletionTimeline(master_semaphore->CurrentTick());
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        upload_cmdbuf.PipelineBarrier(upload_write_barrier);
        upload_cmdbuf.End();
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit2", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    if (static const bool trace = std::getenv("EDEN_NX2_TICK_TRACE") != nullptr; trace) {
        static auto next = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now >= next) {
            next = now + std::chrono::seconds{2};
            size_t queued, reserved;
            {
                std::scoped_lock ql{queue_mutex};
                queued = work_queue.size();
            }
            {
                std::scoped_lock rl{reserve_mutex};
                reserved = chunk_reserve.size();
            }
            LOG_INFO(Render_Vulkan, "Tick trace: current {} gpu {} queued chunks {} reserve {}",
                     master_semaphore->CurrentTick(), master_semaphore->KnownGpuTick(), queued,
                     reserved);
        }
    }
    return signal_value;
}

void Scheduler::InvalidateState() {
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

namespace {
/// The part of a dynamic rendering begin that every pass needs.
struct DynamicRenderingBase {
    std::array<VkImageView, 9> views;
    VkExtent2D render_area;
    u32 num_color;
    u32 layers;
    VkImageView ds_resolve_view;
    VkResolveModeFlagBits depth_resolve_mode;
    VkResolveModeFlagBits stencil_resolve_mode;
    bool has_depth;
    bool has_stencil;
    bool ds_discard;
};

/// The part only a pass that actually clears something needs.
struct DynamicRenderingClears {
    std::array<VkClearValue, 8> color_values;
    VkClearValue ds_value;
    u32 color_clear_mask;
    u32 color_discard_mask;
    bool ds_clear;
};

/// Builds the rendering info and begins the instance. `resolve_views`/`resolve_modes` and `clears`
/// are null for passes with no colour resolve targets and no clears, so those tables never have to
/// be copied into the recorded command.
void IssueBeginRendering(vk::CommandBuffer cmdbuf, const DynamicRenderingBase& base,
                         const std::array<VkImageView, 8>* resolve_views,
                         const std::array<VkResolveModeFlagBits, 8>* resolve_modes,
                         const DynamicRenderingClears* clears) {
    std::array<VkRenderingAttachmentInfo, VideoCommon::NUM_RT> color_infos{};
    for (u32 index = 0; index < base.num_color; ++index) {
        const bool clear_slot =
            clears != nullptr && ((clears->color_clear_mask >> index) & 1u) != 0;
        const VkImageView resolve_view =
            resolve_views != nullptr ? (*resolve_views)[index] : VK_NULL_HANDLE;
        const bool has_resolve = resolve_view != VK_NULL_HANDLE;
        const bool discard_slot =
            has_resolve && clears != nullptr && ((clears->color_discard_mask >> index) & 1u) != 0;
        color_infos[index] = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = base.views[index],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = has_resolve ? (*resolve_modes)[index] : VK_RESOLVE_MODE_NONE,
            .resolveImageView = resolve_view,
            .resolveImageLayout =
                has_resolve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = clear_slot ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = discard_slot ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                    : VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clear_slot ? clears->color_values[index] : VkClearValue{},
        };
    }
    const bool ds_clear = clears != nullptr && clears->ds_clear;
    const VkClearValue ds_clear_value = clears != nullptr ? clears->ds_value : VkClearValue{};
    const bool has_ds_resolve = base.ds_resolve_view != VK_NULL_HANDLE;
    const VkAttachmentLoadOp ds_load_op = ds_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                          : base.ds_discard ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                            : VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkAttachmentStoreOp ds_store_op =
        base.ds_discard ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    const VkRenderingAttachmentInfo depth_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = base.views[8],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = has_ds_resolve ? base.depth_resolve_mode : VK_RESOLVE_MODE_NONE,
        .resolveImageView = has_ds_resolve ? base.ds_resolve_view : VK_NULL_HANDLE,
        .resolveImageLayout = has_ds_resolve ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = ds_load_op,
        .storeOp = ds_store_op,
        .clearValue = ds_clear ? ds_clear_value : VkClearValue{},
    };
    // Stencil gets its own struct because its resolve mode may differ from depth's.
    const VkRenderingAttachmentInfo stencil_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = base.views[8],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = has_ds_resolve ? base.stencil_resolve_mode : VK_RESOLVE_MODE_NONE,
        .resolveImageView = has_ds_resolve ? base.ds_resolve_view : VK_NULL_HANDLE,
        .resolveImageLayout = has_ds_resolve ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = ds_load_op,
        .storeOp = ds_store_op,
        .clearValue = ds_clear ? ds_clear_value : VkClearValue{},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea =
            {
                .offset = {.x = 0, .y = 0},
                .extent = base.render_area,
            },
        .layerCount = base.layers,
        .viewMask = 0,
        .colorAttachmentCount = base.num_color,
        .pColorAttachments = color_infos.data(),
        .pDepthAttachment = base.has_depth ? &depth_info : nullptr,
        .pStencilAttachment = base.has_stencil ? &stencil_info : nullptr,
    };
    cmdbuf.BeginRendering(rendering_info);
}
} // Anonymous namespace

void Scheduler::RecordDynamicBegin(const DeferredClear* clear) {
    const DynamicRenderingBase base{
        .views = state.attachment_views,
        .render_area = state.render_area,
        .num_color = state.num_color,
        .layers = state.layer_count,
        .ds_resolve_view = state.depth_resolve_view,
        .depth_resolve_mode = state.depth_resolve_mode,
        .stencil_resolve_mode = state.stencil_resolve_mode,
        .has_depth = state.has_depth,
        .has_stencil = state.has_stencil,
        .ds_discard = state.discards_msaa_depth,
    };
    bool has_color_resolve = false;
    for (const VkImageView resolve_view : state.color_resolve_views) {
        has_color_resolve = has_color_resolve || resolve_view != VK_NULL_HANDLE;
    }
    if (clear == nullptr && !has_color_resolve) {
        // Common case. Every byte captured here is copied into the command chunk on every single
        // pass begin, and the clear values plus the two resolve tables are two thirds of them.
        Record([base](vk::CommandBuffer cmdbuf) {
            IssueBeginRendering(cmdbuf, base, nullptr, nullptr, nullptr);
        });
        return;
    }
    const DynamicRenderingClears clears{
        .color_values = clear != nullptr ? clear->color_values : std::array<VkClearValue, 8>{},
        .ds_value = clear != nullptr ? clear->depth_stencil_value : VkClearValue{},
        .color_clear_mask = clear != nullptr ? clear->color_clear_mask : 0u,
        .color_discard_mask =
            clear != nullptr && state.discards_msaa_color ? clear->color_clear_mask : 0u,
        .ds_clear = clear != nullptr && clear->depth_stencil,
    };
    Record([base, resolve_views = state.color_resolve_views,
            resolve_modes = state.color_resolve_modes, clears](vk::CommandBuffer cmdbuf) {
        IssueBeginRendering(cmdbuf, base, &resolve_views, &resolve_modes, &clears);
    });
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass() {
    if (state.framebuffer_id == 0) {
        return;
    }
    std::array<VkAccessFlags2, 9> accesses{};
    for (u32 index = 0; index < num_renderpass_images; ++index) {
        accesses[index] = AttachmentWriteAccess(renderpass_image_ranges[index].aspectMask);
    }
    u32 unused_images = 0;
    if (recorded_attachments != nullptr) {
        RelaxAttachmentOps(*recorded_attachments);
        unused_images = RefineImageAccesses(*recorded_attachments, accesses);
    }
    ended_attachments = std::exchange(recorded_attachments, nullptr);

    query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

    // Log render pass end
    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
    }

    query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
    query_cache->NotifySegment(false);

    Record([num_images = num_renderpass_images, images = renderpass_images,
            ranges = renderpass_image_ranges, accesses, unused_images,
            write_barrier = &renderpass_write_barrier,
            num_memory_barriers = static_cast<size_t>(std::exchange(renderpass_writes, false))](
               vk::CommandBuffer cmdbuf) {
        std::array<VkImageMemoryBarrier2, 9> barriers;
        size_t num_barriers = 0;
        for (size_t i = 0; i < num_images; ++i) {
            if (((unused_images >> i) & 1) != 0) {
                continue;
            }
            barriers[num_barriers++] = VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_ATTACHMENTS,
                .srcAccessMask = accesses[i],
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[i],
                .subresourceRange = ranges[i],
            };
        }
        cmdbuf.EndRendering();
        if (num_barriers + num_memory_barriers != 0) {
            cmdbuf.PipelineBarrier(0, vk::Span(write_barrier, num_memory_barriers), {},
                                   vk::Span(barriers.data(), num_barriers));
        }
    });

    state.framebuffer_id = 0;
    alias_framebuffer_id = 0;
    num_renderpass_images = 0;
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = Common::make_unique_for_overwrite<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
