# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/event.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/audio_revision.cpp
    core/applet_id.cpp
    core/program_metadata.cpp
    core/native_gpu_characteristics.cpp
    core/graphics_supervisor.cpp
    core/internal_network/network.cpp
    video_core/native_semaphore.cpp
    video_core/native_shader.cpp
    video_core/native_swizzle.cpp
    video_core/image_overlap.cpp
    video_core/pending_download.cpp
    video_core/memory_tracker.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/ampere_depth_compare.cpp
    video_core/h264_packet.cpp
    video_core/vertex_repack.cpp
    video_core/ampere_lowering.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_gradient.cpp
    video_core/ampere_returns.cpp
    video_core/global_address_tracking.cpp
    video_core/nvn_storage_descriptor.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

if (OPENORBIS)
    target_link_libraries(tests PRIVATE SceVideoOut SceAudioOut ScePad SceSystemService ps4sup)
    create_ps4_eboot(tests tests IV0000-BREW00090_00-DYNARMICTS000000)
endif()
