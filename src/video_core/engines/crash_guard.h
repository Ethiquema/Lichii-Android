// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/*
    Discards invalid segments to avoid issues

    (Ender Magnolia)
    dma_method  subch       mode                segment_method(s)      payload_size     guard           guard_point                           guard_point_alt
    0x06        1-kepler    3-NonIncreasing     0x220(?)                3               Compute         kepler_compute.cpp::ProcessLaunch     vk_rasterizer.cpp::DispatchCompute
    0xE3B       0-macro     5-IncreaseOnce      0xE3A                   5(?)            Draw            macro.cpp::HLE_Draw*                  draw_manager.cpp::ProcessDraw*

    Usage:
    #include "video_core/engines/crash_guard.h"

    //in Maxwell3D::DrawManager::ProcessDraw (after UpdateTopology)
    if (ShouldDiscardCorruptedDraw(draw_state, nullptr, draw_indexed, instance_count)) {
        return;
    }

    //in Maxwell3D::DrawManager::ProcessDrawIndirect (after UpdateTopology)
    if (ShouldDiscardCorruptedDraw(draw_state, &indirect_state, indirect_state.is_indexed, 1)) {
        return;
    }

    //in KeplerCompute::ProcessLaunch (before DispatchCompute)
    if (ShouldDiscardCorruptedCompute(regs)) {
        return;
    }
*/

#pragma once

#include <atomic>
#include <limits>

#include "common/logging.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
//#include "video_core/engines/draw_manager.h"

namespace Tegra::Engines {

[[nodiscard]] inline bool ShouldDiscardCorruptedCompute(const KeplerCompute::LaunchParams& launch_params) {
    // Similar to draw validation, but for compute dispatches. Culling these can fix some compute shader crashes.
    // The main check here is that the grid dimensions are not unreasonably large, which is a common result of corrupted register state.
    constexpr u32 grid_dim_limit = (1u << 16) - 1; // 65536 in each dimension for vulkan, narrower than QMD's X dimension limit of 2^31-1 (Y/Z are equal for both)

    const u32 grid_dim_x = launch_params.grid_dim_x;
    const u32 grid_dim_y = launch_params.grid_dim_y;
    const u32 grid_dim_z = launch_params.grid_dim_z;

    const bool x_exceeded = grid_dim_x > grid_dim_limit;
    const bool y_exceeded = grid_dim_y > grid_dim_limit;
    const bool z_exceeded = grid_dim_z > grid_dim_limit;

    if (x_exceeded || y_exceeded || z_exceeded) {
        LOG_WARNING(HW_GPU, "Discarding compute dispatch with invalid grid dimensions: ({}, {}, {})", grid_dim_x,
                    grid_dim_y, grid_dim_z);

        return true;
    }
    return false;
}

[[nodiscard]] inline bool ShouldDiscardCorruptedDraw(const Maxwell3D::DrawManager::State& draw_state,
                                                     const Maxwell3D::DrawManager::IndirectParams* indirect_state,
                                                     bool draw_indexed, u32 instance_count) {
    constexpr u32 draw_count_limit = 1u << 22; //endermag save screen has > 2^19 valid counts; some titles legitimately exceed 2^20 on indexed indirect (index_buffer.count is an upper bound, not per-draw)
    constexpr u32 instance_count_limit = 1u << 17;
    constexpr u64 draw_span_limit_bytes = 1u << 27;
    // first/base_index/base_instance limits: values above these are garbage register state
    // (e.g. float bit-patterns like 0x3F038000 written into integer fields).
    // No real draw skips >128M indices (first), offsets >16M vertices (base_index),
    // or starts past >1M instances (base_instance).
    constexpr u64 first_limit = 1u << 27;        // 128M index/vertex offset
    constexpr u64 base_index_limit = 1u << 24;   // 16M vertex index offset
    constexpr u64 base_instance_limit = 1u << 20; // 1M instance offset
    constexpr size_t indirect_draw_count_limit = 1u << 18;
    constexpr size_t indirect_buffer_limit = 1u << 24;

    const u64 count = draw_indexed ? static_cast<u64>(draw_state.index_buffer.count)
                                   : static_cast<u64>(draw_state.vertex_buffer.count);
    const u64 first = draw_indexed ? static_cast<u64>(draw_state.index_buffer.first)
                                   : static_cast<u64>(draw_state.vertex_buffer.first);
    const u64 base_index = static_cast<u64>(draw_state.base_index);
    const u64 base_instance = static_cast<u64>(draw_state.base_instance);

    bool index_end_overflow = false;
    bool span_overflow = false;
    bool bounds_invalid = false;
    bool span_exceeds_available = false;

    u64 span_bytes = 0;
    u64 available = 0;
    u64 index_end = first;

    if (draw_indexed) {
        index_end_overflow = first > (std::numeric_limits<u64>::max() - count);
        index_end = index_end_overflow ? std::numeric_limits<u64>::max() : (first + count);

        const u64 format_size = static_cast<u64>(draw_state.index_buffer.FormatSizeInBytes());
        span_overflow = format_size != 0 &&
                        index_end > (std::numeric_limits<u64>::max() / format_size);
        span_bytes = span_overflow ? std::numeric_limits<u64>::max() : (index_end * format_size);

        const GPUVAddr start = draw_state.index_buffer.StartAddress();
        const GPUVAddr end = draw_state.index_buffer.EndAddress();
        bounds_invalid = end < start;
        if (!bounds_invalid) {
            available = (end - start) + 1;
            span_exceeds_available = span_bytes > available;
        }
    }

    // For indexed draws, span_exceeded and span_exceeds_available are the real safety nets —
    // applying a count limit here causes false positives on large legitimate index draws.
    // For non-indexed draws there is no span to check, so count is the only guard.
    const bool validate_count = (indirect_state == nullptr) ? !draw_indexed : draw_indexed;
    const bool count_exceeded = validate_count && count > draw_count_limit;
    const bool instance_exceeded =
        (indirect_state == nullptr) && (static_cast<u64>(instance_count) > instance_count_limit);
    const bool span_exceeded = draw_indexed && span_bytes > draw_span_limit_bytes;
    const bool first_exceeded = first > first_limit;
    const bool base_index_exceeded = base_index > base_index_limit;
    const bool base_instance_exceeded = base_instance > base_instance_limit;

    size_t max_draw_count = 0;
    size_t buffer_size = 0;
    size_t stride = 0;
    bool indirect_count_exceeded = false;
    bool indirect_buffer_exceeded = false;
    bool indirect_stride_exceeded = false;
    bool indirect_shape_invalid = false;

    if (indirect_state != nullptr) {
        max_draw_count = indirect_state->max_draw_counts;
        buffer_size = indirect_state->buffer_size;
        stride = indirect_state->stride;

        indirect_count_exceeded = max_draw_count > indirect_draw_count_limit;
        indirect_buffer_exceeded = buffer_size > indirect_buffer_limit;
        indirect_stride_exceeded = stride > draw_span_limit_bytes;

        if (!indirect_state->is_byte_count && max_draw_count > 1) {
            if (stride == 0) {
                indirect_shape_invalid = true;
            } else {
                const size_t command_size =
                    indirect_state->is_indexed ? (5 * sizeof(u32)) : (4 * sizeof(u32));
                const bool draw_tail_overflow =
                    (max_draw_count - 1) > (std::numeric_limits<size_t>::max() / stride);
                const size_t draw_tail =
                    draw_tail_overflow ? std::numeric_limits<size_t>::max()
                                       : ((max_draw_count - 1) * stride);
                const bool needed_overflow =
                    draw_tail_overflow ||
                    draw_tail > (std::numeric_limits<size_t>::max() - command_size);
                const size_t needed_size =
                    needed_overflow ? std::numeric_limits<size_t>::max()
                                    : (draw_tail + command_size);
                indirect_shape_invalid = needed_overflow || needed_size > buffer_size;
            }
        }
    }

    const bool discard = count_exceeded || instance_exceeded || span_exceeded ||
                         index_end_overflow || span_overflow || bounds_invalid ||
                         span_exceeds_available || first_exceeded || base_index_exceeded ||
                         base_instance_exceeded || indirect_count_exceeded ||
                         indirect_buffer_exceeded || indirect_stride_exceeded ||
                         indirect_shape_invalid;
    if (!discard) {
        return false;
    }

    LOG_WARNING(
        HW_GPU,
        "DrawManager: blocked {} draw path={} count={} limit={} first={:#x} "
        "base_index={:#x} base_instance={:#x} span_bytes={} available={} "
        "flags(count_exceeded={} instance_exceeded={} span_exceeded={} "
        "index_end_overflow={} span_overflow={} bounds_invalid={} span_exceeds_available={} "
        "first_exceeded={} base_index_exceeded={} base_instance_exceeded={} "
        "indirect_count_exceeded={} indirect_buffer_exceeded={} "
        "indirect_stride_exceeded={} indirect_shape_invalid={}) "
        "limits(count={} first={:#x} base_index={:#x} base_instance={:#x}) "
        "max_draw_count={} buffer_size={} indirect_limits(count={} buffer={})",
        draw_indexed ? "indexed" : "vertex",
        indirect_state != nullptr ? "indirect" : "direct",
        count, draw_count_limit, first, base_index, base_instance, span_bytes, available,
        count_exceeded ? 1 : 0, instance_exceeded ? 1 : 0, span_exceeded ? 1 : 0,
        index_end_overflow ? 1 : 0, span_overflow ? 1 : 0, bounds_invalid ? 1 : 0,
        span_exceeds_available ? 1 : 0,
        first_exceeded ? 1 : 0, base_index_exceeded ? 1 : 0, base_instance_exceeded ? 1 : 0,
        indirect_count_exceeded ? 1 : 0, indirect_buffer_exceeded ? 1 : 0,
        indirect_stride_exceeded ? 1 : 0, indirect_shape_invalid ? 1 : 0,
        draw_span_limit_bytes, first_limit, base_index_limit, base_instance_limit,
        max_draw_count, buffer_size, indirect_draw_count_limit, indirect_buffer_limit);

    return true;
}

} // namespace Tegra::Engines