// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <optional>
#include <set>
#include <span>
#include <string>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaPool)

// Define all features which may be used by the implementation here.
// Vulkan version in the macro describes the minimum version required for feature availability.
// If the Vulkan version is lower than the required version, the named extension is required.
#define FOR_EACH_VK_FEATURE_1_1(FEATURE)                                                           \
    FEATURE(KHR, 16BitStorage, 16BIT_STORAGE, bit16_storage)                                       \
    FEATURE(KHR, ShaderAtomicInt64, SHADER_ATOMIC_INT64, shader_atomic_int64)                      \
    FEATURE(KHR, ShaderDrawParameters, SHADER_DRAW_PARAMETERS, shader_draw_parameters)             \
    FEATURE(KHR, ShaderFloat16Int8, SHADER_FLOAT16_INT8, shader_float16_int8)                      \
    FEATURE(KHR, UniformBufferStandardLayout, UNIFORM_BUFFER_STANDARD_LAYOUT,                      \
            uniform_buffer_standard_layout)                                                        \
    FEATURE(KHR, VariablePointer, VARIABLE_POINTERS, variable_pointer)

#define FOR_EACH_VK_FEATURE_1_2(FEATURE)                                                           \
    FEATURE(EXT, DescriptorIndexing, DESCRIPTOR_INDEXING, descriptor_indexing)                     \
    FEATURE(EXT, HostQueryReset, HOST_QUERY_RESET, host_query_reset)                               \
    FEATURE(KHR, 8BitStorage, 8BIT_STORAGE, bit8_storage)                                          \
    FEATURE(KHR, BufferDeviceAddress, BUFFER_DEVICE_ADDRESS, buffer_device_address)                \
    FEATURE(KHR, TimelineSemaphore, TIMELINE_SEMAPHORE, timeline_semaphore)                        \
    FEATURE(KHR, VulkanMemoryModel, VULKAN_MEMORY_MODEL, vulkan_memory_model)

#define FOR_EACH_VK_FEATURE_1_3(FEATURE)                                                           \
    FEATURE(KHR, DynamicRendering, DYNAMIC_RENDERING, dynamic_rendering)                           \
    FEATURE(EXT, ImageRobustness, IMAGE_ROBUSTNESS, robust_image_access)                           \
    FEATURE(EXT, ShaderDemoteToHelperInvocation, SHADER_DEMOTE_TO_HELPER_INVOCATION,               \
            shader_demote_to_helper_invocation)                                                    \
    FEATURE(EXT, SubgroupSizeControl, SUBGROUP_SIZE_CONTROL, subgroup_size_control)                \
    FEATURE(KHR, DynamicRendering, DYNAMIC_RENDERING, dynamic_rendering)                           \
    FEATURE(KHR, Maintenance4, MAINTENANCE_4, maintenance4)                                        \
    FEATURE(KHR, Synchronization2, SYNCHRONIZATION_2, synchronization2)

#define FOR_EACH_VK_FEATURE_1_4(FEATURE)

// Define all features which may be used by the implementation and require an extension here.
#define FOR_EACH_VK_FEATURE_EXT(FEATURE)                                                           \
    FEATURE(EXT, BorderColorSwizzle, BORDER_COLOR_SWIZZLE, border_color_swizzle)                   \
    FEATURE(EXT, ColorWriteEnable, COLOR_WRITE_ENABLE, color_write_enable)                         \
    FEATURE(EXT, ConditionalRendering, CONDITIONAL_RENDERING, conditional_rendering)               \
    FEATURE(EXT, CustomBorderColor, CUSTOM_BORDER_COLOR, custom_border_color)                      \
    FEATURE(EXT, DepthBiasControl, DEPTH_BIAS_CONTROL, depth_bias_control)                         \
    FEATURE(EXT, DepthClampZeroOne, DEPTH_CLAMP_ZERO_ONE, depth_clamp_zero_one)                   \
    FEATURE(EXT, DepthClipControl, DEPTH_CLIP_CONTROL, depth_clip_control)                         \
    FEATURE(EXT, DepthClipEnable, DEPTH_CLIP_ENABLE, depth_clip_enable)                            \
    FEATURE(EXT, DescriptorBuffer, DESCRIPTOR_BUFFER, descriptor_buffer)                           \
    FEATURE(EXT, DynamicRenderingUnusedAttachments, DYNAMIC_RENDERING_UNUSED_ATTACHMENTS,          \
            dynamic_rendering_unused_attachments)                                                  \
    FEATURE(EXT, ExtendedDynamicState, EXTENDED_DYNAMIC_STATE, extended_dynamic_state)             \
    FEATURE(EXT, ExtendedDynamicState2, EXTENDED_DYNAMIC_STATE_2, extended_dynamic_state2)         \
    FEATURE(EXT, ExtendedDynamicState3, EXTENDED_DYNAMIC_STATE_3, extended_dynamic_state3)         \
    FEATURE(EXT, 4444Formats, 4444_FORMATS, format_a4b4g4r4)                                       \
    FEATURE(EXT, HostImageCopy, HOST_IMAGE_COPY, host_image_copy)                                  \
    FEATURE(EXT, IndexTypeUint8, INDEX_TYPE_UINT8, index_type_uint8)                               \
    FEATURE(EXT, LineRasterization, LINE_RASTERIZATION, line_rasterization)                        \
    FEATURE(EXT, MeshShader, MESH_SHADER, mesh_shader)                                             \
    FEATURE(EXT, PrimitiveTopologyListRestart, PRIMITIVE_TOPOLOGY_LIST_RESTART,                    \
            primitive_topology_list_restart)                                                       \
    FEATURE(EXT, ProvokingVertex, PROVOKING_VERTEX, provoking_vertex)                              \
    FEATURE(EXT, Robustness2, ROBUSTNESS_2, robustness2)                                           \
    FEATURE(EXT, TexelBufferAlignment, TEXEL_BUFFER_ALIGNMENT, texel_buffer_alignment)             \
    FEATURE(EXT, TransformFeedback, TRANSFORM_FEEDBACK, transform_feedback)                        \
    FEATURE(EXT, VertexAttributeDivisor, VERTEX_ATTRIBUTE_DIVISOR, vertex_attribute_divisor)       \
    FEATURE(EXT, VertexInputDynamicState, VERTEX_INPUT_DYNAMIC_STATE, vertex_input_dynamic_state)  \
    FEATURE(KHR, Maintenance5, MAINTENANCE_5, maintenance5)                                        \
    FEATURE(KHR, PipelineExecutableProperties, PIPELINE_EXECUTABLE_PROPERTIES,                     \
            pipeline_executable_properties)                                                        \
    FEATURE(KHR, ShaderQuadControl, SHADER_QUAD_CONTROL, shader_quad_control)                      \
    FEATURE(KHR, WorkgroupMemoryExplicitLayout, WORKGROUP_MEMORY_EXPLICIT_LAYOUT,                  \
            workgroup_memory_explicit_layout)                                                      \
    FEATURE(EXT, TextureCompressionASTCHDR, TEXTURE_COMPRESSION_ASTC_HDR,                          \
            texture_compression_astc_hdr)


// Define miscellaneous extensions which may be used by the implementation here.
#define FOR_EACH_VK_EXTENSION(EXTENSION)                                                           \
    EXTENSION(EXT, ASTC_DECODE_MODE, astc_decode_mode)                                             \
    EXTENSION(EXT, CONSERVATIVE_RASTERIZATION, conservative_rasterization)                         \
    EXTENSION(EXT, DEPTH_RANGE_UNRESTRICTED, depth_range_unrestricted)                             \
    EXTENSION(EXT, EXTERNAL_MEMORY_HOST, external_memory_host)                                     \
    EXTENSION(EXT, MEMORY_BUDGET, memory_budget)                                                   \
    EXTENSION(EXT, PIPELINE_CREATION_FEEDBACK, pipeline_creation_feedback)                         \
    EXTENSION(EXT, ROBUSTNESS_2, robustness_2)                                                     \
    EXTENSION(EXT, SAMPLER_FILTER_MINMAX, sampler_filter_minmax)                                   \
    EXTENSION(EXT, SHADER_STENCIL_EXPORT, shader_stencil_export)                                   \
    EXTENSION(EXT, SHADER_VIEWPORT_INDEX_LAYER, shader_viewport_index_layer)                       \
    EXTENSION(EXT, TOOLING_INFO, tooling_info)                                                     \
    EXTENSION(KHR, COPY_COMMANDS_2, copy_commands2)                                                \
    EXTENSION(KHR, CREATE_RENDERPASS_2, create_renderpass2)                                        \
    EXTENSION(KHR, DEPTH_STENCIL_RESOLVE, depth_stencil_resolve)                                   \
=======
    EXTENSION(EXT, DEVICE_FAULT, device_fault)                                                     \
>>>>>>> /tmp/tmpctn1l5_p/t
    EXTENSION(KHR, DRAW_INDIRECT_COUNT, draw_indirect_count)                                       \
    EXTENSION(KHR, DRIVER_PROPERTIES, driver_properties)                                           \
    EXTENSION(KHR, PUSH_DESCRIPTOR, push_descriptor)                                               \
    EXTENSION(KHR, SAMPLER_MIRROR_CLAMP_TO_EDGE, sampler_mirror_clamp_to_edge)                     \
    EXTENSION(KHR, SHADER_FLOAT_CONTROLS, shader_float_controls)                                   \
    EXTENSION(KHR, SPIRV_1_4, spirv_1_4)                                                           \
    EXTENSION(KHR, SWAPCHAIN, swapchain)                                                           \
    EXTENSION(KHR, SWAPCHAIN_MUTABLE_FORMAT, swapchain_mutable_format)                             \
    EXTENSION(KHR, IMAGE_FORMAT_LIST, image_format_list)                                           \
    EXTENSION(KHR, LOAD_STORE_OP_NONE, load_store_op_none)                                         \
    EXTENSION(EXT, LOAD_STORE_OP_NONE, load_store_op_none_ext)                                     \
    EXTENSION(NV, DEVICE_DIAGNOSTICS_CONFIG, device_diagnostics_config)                            \
    EXTENSION(NV, GEOMETRY_SHADER_PASSTHROUGH, geometry_shader_passthrough)                        \
    EXTENSION(NV, VIEWPORT_ARRAY2, viewport_array2)                                                \
    EXTENSION(NV, VIEWPORT_SWIZZLE, viewport_swizzle)                                              \
    EXTENSION(EXT, FILTER_CUBIC, filter_cubic)                                                     \
    EXTENSION(QCOM, FILTER_CUBIC_WEIGHTS, filter_cubic_weights)

// Define extensions which must be supported.
#define FOR_EACH_VK_MANDATORY_EXTENSION(EXTENSION_NAME)                                            \
    EXTENSION_NAME(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)                                        \
    EXTENSION_NAME(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)                             \
    EXTENSION_NAME(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)

// Define extensions where the absence of the extension may result in a degraded experience.
#define FOR_EACH_VK_RECOMMENDED_EXTENSION(EXTENSION_NAME)                                          \
    EXTENSION_NAME(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)                                    \
    EXTENSION_NAME(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)                                       \
    EXTENSION_NAME(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)                                   \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)                                 \
    EXTENSION_NAME(VK_EXT_4444_FORMATS_EXTENSION_NAME)                                             \
    EXTENSION_NAME(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME)                                       \
    EXTENSION_NAME(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)                                             \
    EXTENSION_NAME(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME)                               \
    EXTENSION_NAME(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME)                                           \
    EXTENSION_NAME(VK_NV_VIEWPORT_SWIZZLE_EXTENSION_NAME)

// Define features which must be supported.
#define FOR_EACH_VK_MANDATORY_FEATURE(FEATURE_NAME)                                                \
    FEATURE_NAME(features, depthBiasClamp)                                                         \
    FEATURE_NAME(features, depthClamp)                                                             \
    FEATURE_NAME(features, drawIndirectFirstInstance)                                              \
    FEATURE_NAME(features, dualSrcBlend)                                                           \
    FEATURE_NAME(features, fillModeNonSolid)                                                       \
    FEATURE_NAME(features, fragmentStoresAndAtomics)                                               \
    FEATURE_NAME(features, geometryShader)                                                         \
    FEATURE_NAME(features, imageCubeArray)                                                         \
    FEATURE_NAME(features, independentBlend)                                                       \
    FEATURE_NAME(features, largePoints)                                                            \
    FEATURE_NAME(features, logicOp)                                                                \
    FEATURE_NAME(features, multiDrawIndirect)                                                      \
    FEATURE_NAME(features, multiViewport)                                                          \
    FEATURE_NAME(features, occlusionQueryPrecise)                                                  \
    FEATURE_NAME(features, robustBufferAccess)                                                     \
    FEATURE_NAME(features, samplerAnisotropy)                                                      \
    FEATURE_NAME(features, sampleRateShading)                                                      \
    FEATURE_NAME(features, shaderClipDistance)                                                     \
    FEATURE_NAME(features, shaderCullDistance)                                                     \
    FEATURE_NAME(features, shaderImageGatherExtended)                                              \
    FEATURE_NAME(features, shaderStorageImageWriteWithoutFormat)                                   \
    FEATURE_NAME(features, tessellationShader)                                                     \
    FEATURE_NAME(features, vertexPipelineStoresAndAtomics)                                         \
    FEATURE_NAME(features, wideLines)                                                              \
    FEATURE_NAME(host_query_reset, hostQueryReset)                                                 \
    FEATURE_NAME(shader_demote_to_helper_invocation, shaderDemoteToHelperInvocation)               \
    FEATURE_NAME(shader_draw_parameters, shaderDrawParameters)                                     \
    FEATURE_NAME(variable_pointer, variablePointers)                                               \
    FEATURE_NAME(variable_pointer, variablePointersStorageBuffer)

// Define features where the absence of the feature may result in a degraded experience.
#define FOR_EACH_VK_RECOMMENDED_FEATURE(FEATURE_NAME)                                              \
    FEATURE_NAME(bit16_storage, storageBuffer16BitAccess)                                          \
    FEATURE_NAME(bit8_storage, storageBuffer8BitAccess)                                            \
    FEATURE_NAME(custom_border_color, customBorderColors)                                          \
    FEATURE_NAME(depth_bias_control, depthBiasControl)                                             \
    FEATURE_NAME(depth_bias_control, leastRepresentableValueForceUnormRepresentation)              \
    FEATURE_NAME(depth_bias_control, depthBiasExact)                                               \
    FEATURE_NAME(descriptor_indexing, descriptorBindingPartiallyBound)                             \
    FEATURE_NAME(descriptor_indexing, shaderSampledImageArrayNonUniformIndexing)                   \
    FEATURE_NAME(extended_dynamic_state, extendedDynamicState)                                     \
    FEATURE_NAME(format_a4b4g4r4, formatA4B4G4R4)                                                  \
    FEATURE_NAME(robust_image_access, robustImageAccess)                                           \
    FEATURE_NAME(index_type_uint8, indexTypeUint8)                                                 \
    FEATURE_NAME(primitive_topology_list_restart, primitiveTopologyListRestart)                    \
    FEATURE_NAME(provoking_vertex, provokingVertexLast)                                            \
    FEATURE_NAME(robustness2, nullDescriptor)                                                      \
    FEATURE_NAME(shader_float16_int8, shaderFloat16)                                               \
    FEATURE_NAME(shader_float16_int8, shaderInt8)                                                  \
    FEATURE_NAME(synchronization2, synchronization2)                                               \
    FEATURE_NAME(timeline_semaphore, timelineSemaphore)                                            \
    FEATURE_NAME(transform_feedback, transformFeedback)                                            \
    FEATURE_NAME(uniform_buffer_standard_layout, uniformBufferStandardLayout)                      \
    FEATURE_NAME(vertex_input_dynamic_state, vertexInputDynamicState)

// These features are not required but can be helpful for drivers that can use it.
#define FOR_EACH_VK_OPTIONAL_FEATURE(FEATURE_NAME)                                                 \
    FEATURE_NAME(bit16_storage, uniformAndStorageBuffer16BitAccess)                                \
    FEATURE_NAME(bit8_storage, uniformAndStorageBuffer8BitAccess)

namespace Vulkan {

class NsightAftermathTracker;

/// Format usage descriptor.
enum class FormatType { Linear, Optimal, Buffer };

/// Subgroup size of the guest emulated hardware (Nvidia has 32 threads per subgroup).
const u32 GuestWarpSize = 32;

enum class NvidiaArchitecture {
    Arch_KeplerOrOlder,
    Arch_Maxwell,
    Arch_Pascal,
    Arch_Volta,
    Arch_Turing,
    Arch_AmpereOrNewer,
};

/// Handles data specific to a physical device.
class Device {
public:
    explicit Device(VkInstance instance, vk::PhysicalDevice physical, VkSurfaceKHR surface,
                    const vk::InstanceDispatch& dld);
    ~Device();

    /**
     * Returns a format supported by the device for the passed requirements.
     * @param wanted_format The ideal format to be returned. It may not be the returned format.
     * @param wanted_usage The usage that must be fulfilled even if the format is not supported.
     * @param format_type Format type usage.
     * @returns A format supported by the device.
     */
    VkFormat GetSupportedFormat(VkFormat wanted_format, VkFormatFeatureFlags wanted_usage,
                                FormatType format_type) const;

    /// Returns true if a format is supported.
    bool IsFormatSupported(VkFormat wanted_format, VkFormatFeatureFlags wanted_usage,
                           FormatType format_type) const;

    /// Reports a device loss.
    void ReportLoss() const;

    /// Reports a shader to Nsight Aftermath.
    void SaveShader(std::span<const u32> spirv) const;

    /// Returns the name of the VkDriverId reported from Vulkan.
    std::string GetDriverName() const;

    /// Returns the dispatch loader with direct function pointers of the device.
    const vk::DeviceDispatch& GetDispatchLoader() const {
        return dld;
    }

    /// Returns the VMA allocator.
    VmaAllocator GetAllocator() const {
        return allocator;
    }

    struct MemoryBudgetInfo {
        u64 usage;
        u64 budget;
        u64 block_bytes;
        u64 allocation_bytes;
    };

    MemoryBudgetInfo GetMemoryBudgetInfo() const;

    void TickAllocatorFrame() const;

    /// Returns the logical device.
    const vk::Device& GetLogical() const {
        return logical;
    }

    /// Returns the physical device.
    vk::PhysicalDevice GetPhysical() const {
        return physical;
    }

    /// Returns the main graphics queue.
    vk::Queue GetGraphicsQueue() const {
        return graphics_queue;
    }

    /// Returns the main present queue.
    vk::Queue GetPresentQueue() const {
        return present_queue;
    }

    /// Returns main graphics queue family index.
    u32 GetGraphicsFamily() const {
        return graphics_family;
    }

    /// Returns main present queue family index.
    u32 GetPresentFamily() const {
        return present_family;
    }

    /// Returns the current Vulkan API version provided in Vulkan-formatted version numbers.
    u32 ApiVersion() const {
        return properties.properties.apiVersion;
    }

    VkDeviceSize TexelBufferAlignment(u32 texel_size) const {
        VkDeviceSize texel = texel_size;
        if (texel % 3 == 0) {
            texel /= 3;
        }
        const auto& limits = properties.texel_buffer_alignment;
        VkDeviceSize storage = limits.storageTexelBufferOffsetAlignmentBytes;
        if (limits.storageTexelBufferOffsetSingleTexelAlignment != VK_FALSE) {
            storage = (std::min)(storage, texel);
        }
        VkDeviceSize uniform = limits.uniformTexelBufferOffsetAlignmentBytes;
        if (limits.uniformTexelBufferOffsetSingleTexelAlignment != VK_FALSE) {
            uniform = (std::min)(uniform, texel);
        }
        return (std::max)((std::max)(storage, uniform), VkDeviceSize{1});
    }

    /// Returns the current driver version provided in Vulkan-formatted version numbers.
    u32 GetDriverVersion() const {
        return properties.properties.driverVersion;
    }

    /// Returns the device name.
    std::string_view GetModelName() const {
        return properties.properties.deviceName;
    }

    /// Returns the driver ID.
    VkDriverIdKHR GetDriverID() const {
        return properties.driver.driverID;
    }

    bool IsSparseBindingSupported() const {
        return features.features.sparseBinding && graphics_family_sparse_binding;
    }

    /// 2D images whose memory can be bound in parts, on the graphics queue.
    bool IsSparseResidencyImage2DSupported() const {
        return IsSparseBindingSupported() && features.features.sparseResidencyImage2D;
    }

    /// Whether a sparse image may bind the same memory to several of its blocks.
    bool IsSparseResidencyAliasedSupported() const {
        return IsSparseResidencyImage2DSupported() && features.features.sparseResidencyAliased;
    }

    /// Whether images with no memory bound may be sampled and used as attachments. AMD's drivers
    /// fault on it (the GPU hangs shortly after the first such draw), so large images there are
    /// backed normally. RADV is counted in until someone shows it doesn't need it.
    /// EDEN_NX2_SPARSE_UNBACKED=1 allows unbound images on any driver, =0 on none.
    bool CanUseUnbackedSparseImages() const {
        static const char forced = [] {
            const char* value = std::getenv("EDEN_NX2_SPARSE_UNBACKED");
            return value == nullptr ? '\0' : value[0];
        }();
        if (forced == '0' || forced == '1') {
            return forced == '1';
        }
        const VkDriverIdKHR id = GetDriverID();
        return id != VK_DRIVER_ID_AMD_PROPRIETARY && id != VK_DRIVER_ID_AMD_OPEN_SOURCE &&
               id != VK_DRIVER_ID_MESA_RADV;
    }

    /// Returns true for tile-based deferred renderers.
    bool IsTiler() const {
        switch (GetDriverID()) {
        case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:
        case VK_DRIVER_ID_ARM_PROPRIETARY:
        case VK_DRIVER_ID_SAMSUNG_PROPRIETARY:
        case VK_DRIVER_ID_MESA_TURNIP:
            return true;
        default:
            return false;
        }
    }

    bool ShouldBoostClocks() const;

    /// Returns uniform buffer alignment requirement.
    VkDeviceSize GetUniformBufferAlignment() const {
        return properties.properties.limits.minUniformBufferOffsetAlignment;
    }

    /// Returns storage alignment requirement.
    VkDeviceSize GetStorageBufferAlignment() const {
        return properties.properties.limits.minStorageBufferOffsetAlignment;
    }

    /// Returns the maximum range for storage buffers.
    VkDeviceSize GetMaxStorageBufferRange() const {
        return properties.properties.limits.maxStorageBufferRange;
    }

    std::array<u32, 3> GetMaxComputeWorkGroupCount() const {
        const auto& count = properties.properties.limits.maxComputeWorkGroupCount;
        return {count[0], count[1], count[2]};
    }

#define FN_MAX_LIMIT_LIST \
    FN_MAX_LIMIT_ELEM(ComputeSharedMemorySize) \
    FN_MAX_LIMIT_ELEM(ComputeWorkGroupInvocations) \
    FN_MAX_LIMIT_ELEM(PerStageDescriptorSampledImages) \
    FN_MAX_LIMIT_ELEM(PerStageDescriptorStorageBuffers) \
    FN_MAX_LIMIT_ELEM(PerStageResources) \
    FN_MAX_LIMIT_ELEM(DescriptorSetSamplers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetUniformBuffers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetUniformBuffersDynamic) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageBuffers) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageBuffersDynamic) \
    FN_MAX_LIMIT_ELEM(DescriptorSetSampledImages) \
    FN_MAX_LIMIT_ELEM(DescriptorSetStorageImages) \
    FN_MAX_LIMIT_ELEM(DescriptorSetInputAttachments)
#define FN_MAX_LIMIT_ELEM(name) \
    u32 GetMax##name() const { return properties.properties.limits.max##name; }
FN_MAX_LIMIT_LIST
#undef FN_MAX_LIMIT_ELEM
#undef FN_MAX_LIMIT_LIST

    /// Returns float control properties of the device.
    const VkPhysicalDeviceFloatControlsPropertiesKHR& FloatControlProperties() const {
        return properties.float_controls;
    }

    /// Returns true if ASTC is natively supported, including HDR-profile (non-LDR-only)
    bool IsOptimalAstcSupported() const {
        return is_optimal_astc_supported;
    }

    /// Returns true if BCn is natively supported.
    bool IsOptimalBcnSupported() const {
        return features.features.textureCompressionBC;
    }

    /// Returns true if ETC2 is natively supported.
    bool IsOptimalEtc2Supported() const {
        return features.features.textureCompressionETC2;
    }

    /// Returns true if descriptor aliasing is natively supported.
    bool IsDescriptorAliasingSupported() const {
        return features.descriptor_indexing.descriptorBindingPartiallyBound &&
               features.descriptor_indexing.descriptorBindingVariableDescriptorCount;
    }

    bool IsSampledImageArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderSampledImageArrayNonUniformIndexing;
    }

    bool IsStorageImageArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderStorageImageArrayNonUniformIndexing;
    }

    bool IsUniformTexelBufferArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing;
    }

    bool IsStorageTexelBufferArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing;
    }

    bool IsStorageBufferArrayNonUniformIndexingSupported() const {
        return features.descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing;
    }

    /// Returns true if the device supports float64 natively.
    bool IsFloat64Supported() const {
        return features.features.shaderFloat64;
    }

    /// Returns true if the device supports float16 natively.
    bool IsFloat16Supported() const {
        return features.shader_float16_int8.shaderFloat16 && !has_broken_float16_math;
    }

    bool IsNativeFloat16Enabled() const {
        return features.shader_float16_int8.shaderFloat16;
    }

    /// Returns true if the device can run shaders built against the Vulkan memory model.
    bool IsVulkanMemoryModelSupported() const {
        return features.vulkan_memory_model.vulkanMemoryModel;
    }

    /// Returns true if the device supports int8 natively.
    bool IsInt8Supported() const {
        return features.shader_float16_int8.shaderInt8;
    }

    /// Returns true if the device allows 8-bit integer members in uniform/storage buffers.
    bool IsUniformAndStorageBuffer8BitAccessSupported() const {
        return features.bit8_storage.uniformAndStorageBuffer8BitAccess;
    }

    /// Returns true if the device allows 16-bit integer members in uniform/storage buffers.
    bool IsUniformAndStorageBuffer16BitAccessSupported() const {
        return features.bit16_storage.uniformAndStorageBuffer16BitAccess;
    }

    /// Returns true if the device supports reading 8-bit values from a storage buffer.
    bool IsStorageBuffer8BitAccessSupported() const {
        return features.bit8_storage.storageBuffer8BitAccess;
    }

    /// Returns true if the device supports reading 16-bit values from a storage buffer.
    bool IsStorageBuffer16BitAccessSupported() const {
        return features.bit16_storage.storageBuffer16BitAccess;
    }

    /// Returns true if the device supports binding multisample images as storage images.
    bool IsStorageImageMultisampleSupported() const {
        return features.features.shaderStorageImageMultisample;
    }

    /// Returns true if the device warp size can potentially be bigger than guest's warp size.
    bool IsWarpSizePotentiallyBiggerThanGuest() const {
        return is_warp_potentially_bigger;
    }

    /// Returns true if the device can be forced to use the guest warp size.
    bool IsGuestWarpSizeSupported(VkShaderStageFlagBits stage) const {
        return properties.subgroup_size_control.requiredSubgroupSizeStages & stage;
    }

    /// Returns true if the device supports the provided subgroup feature.
    bool IsSubgroupFeatureSupported(VkSubgroupFeatureFlagBits feature) const {
        return properties.subgroup_properties.supportedOperations & feature;
    }

    VkShaderStageFlags GetSubgroupSupportedStages() const {
        return properties.subgroup_properties.supportedStages;
    }

    u32 GetMaxSubgroupSize() const {
        return (std::max)(properties.subgroup_properties.subgroupSize,
                          properties.subgroup_size_control.maxSubgroupSize);
    }

    /// Returns the maximum number of push descriptors.
    u32 MaxPushDescriptors() const {
        return properties.push_descriptor.maxPushDescriptors;
    }

    /// Returns true if the device supports descriptor buffers.
    bool IsExtPipelineCreationFeedbackSupported() const {
        return extensions.pipeline_creation_feedback;
    }

    bool IsRobustBufferAccessEnabled() const {
        return features.features.robustBufferAccess == VK_TRUE;
    }

    bool IsExtDescriptorBufferSupported() const {
        return extensions.descriptor_buffer;
    }

    /// Returns the descriptor buffer properties of the device.
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& DescriptorBufferProperties() const {
        return properties.descriptor_buffer;
    }

    /// Returns true if the device supports buffer device address.
    bool IsBufferDeviceAddressSupported() const {
        return extensions.buffer_device_address;
    }

    /// Returns true if formatless image load is supported.
    bool IsFormatlessImageLoadSupported() const {
        return features.features.shaderStorageImageReadWithoutFormat;
    }

    /// Returns true if shader int64 is supported.
    bool IsShaderInt64Supported() const {
        return features.features.shaderInt64;
    }

    /// Returns true if shader int16 is supported.
    bool IsShaderInt16Supported() const {
        return features.features.shaderInt16;
    }

    // Returns true if depth bounds is supported.
    bool IsDepthBoundsSupported() const {
        return features.features.depthBounds;
    }

    /// Returns true when blitting from and to D24S8 images is supported.
    bool IsBlitDepth24Stencil8Supported() const {
        return is_blit_depth24_stencil8_supported;
    }

    /// Returns true when blitting from and to D32S8 images is supported.
    bool IsBlitDepth32Stencil8Supported() const {
        return is_blit_depth32_stencil8_supported;
    }

    /// Returns true if the device supports VK_NV_viewport_swizzle.
    bool IsNvViewportSwizzleSupported() const {
        return extensions.viewport_swizzle;
    }

    /// Returns true if the device supports VK_NV_viewport_array2.
    bool IsNvViewportArray2Supported() const {
        return extensions.viewport_array2;
    }

    /// Returns true if the device supports VK_NV_geometry_shader_passthrough.
    bool IsNvGeometryShaderPassthroughSupported() const {
        return extensions.geometry_shader_passthrough;
    }

    /// Returns true if the device supports VK_KHR_push_descriptor.
    bool IsKhrPushDescriptorSupported() const {
        return extensions.push_descriptor;
    }

    /// Returns true if VK_KHR_pipeline_executable_properties is enabled.
    bool IsKhrPipelineExecutablePropertiesEnabled() const {
        return extensions.pipeline_executable_properties;
    }

    /// Returns true if VK_KHR_swapchain_mutable_format is enabled.
    bool IsKhrSwapchainMutableFormatEnabled() const {
        return extensions.swapchain_mutable_format;
    }

    /// Returns true if VK_KHR_shader_float_controls is enabled.
    bool IsKhrShaderFloatControlsSupported() const {
        return extensions.shader_float_controls;
    }

    /// Returns true if VK_KHR_maintenance1 is enabled.
    bool IsKhrMaintenance1Supported() const {
        return extensions.maintenance1;
    }

    /// Returns true if VK_KHR_maintenance2 is enabled.
    bool IsKhrMaintenance2Supported() const {
        return extensions.maintenance2;
    }

    /// Returns true if VK_KHR_maintenance3 is enabled.
    bool IsKhrMaintenance3Supported() const {
        return extensions.maintenance3;
    }

    /// Returns true if VK_KHR_maintenance4 is enabled.
    bool IsKhrMaintenance4Supported() const {
        return extensions.maintenance4;
    }

    /// Returns true if VK_KHR_maintenance5 is enabled.
    bool IsKhrMaintenance5Supported() const {
        return extensions.maintenance5;
    }

    /// Returns true if VK_KHR_maintenance6 is enabled.
    bool IsKhrMaintenance6Supported() const {
        return extensions.maintenance6;
    }

    /// Returns true if VK_KHR_maintenance7 is enabled.
    bool IsKhrMaintenance7Supported() const {
        return extensions.maintenance7;
    }

    /// Returns true if VK_KHR_maintenance8 is enabled.
    bool IsKhrMaintenance8Supported() const {
        return extensions.maintenance8;
    }

    /// Returns true if VK_KHR_maintenance9 is enabled.
    bool IsKhrMaintenance9Supported() const {
        return extensions.maintenance9;
    }

    /// Returns true if VK_KHR_sampler_mirror_clamp_to_edge is enabled.
    bool IsKhrSamplerMirrorClampToEdgeSupported() const {
        return extensions.sampler_mirror_clamp_to_edge;
    }

    /// Returns true if the device supports VK_KHR_workgroup_memory_explicit_layout.
    bool IsKhrWorkgroupMemoryExplicitLayoutSupported() const {
        return extensions.workgroup_memory_explicit_layout;
    }

    /// Returns true if the device supports 8-bit accesses to workgroup explicit layout memory.
    bool IsWorkgroupMemoryExplicitLayout8BitSupported() const {
        return extensions.workgroup_memory_explicit_layout &&
               features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout8BitAccess;
    }

    /// Returns true if the device supports 16-bit accesses to workgroup explicit layout memory.
    bool IsWorkgroupMemoryExplicitLayout16BitSupported() const {
        return extensions.workgroup_memory_explicit_layout && features.features.shaderInt16 &&
               features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout16BitAccess;
    }
 
    /// Returns true if the device supports VK_KHR_shader_quad_control.
    bool IsKhrShaderQuadControlSupported() const {
        return extensions.shader_quad_control && features.shader_quad_control.shaderQuadControl;
    }

    /// Returns true if the device supports VK_KHR_image_format_list.
    bool IsKhrImageFormatListSupported() const {
        return extensions.image_format_list || instance_version >= VK_API_VERSION_1_2;
    }

    /// Returns true if the device supports VK_EXT_primitive_topology_list_restart.
    bool IsTopologyListPrimitiveRestartSupported() const {
        return features.primitive_topology_list_restart.primitiveTopologyListRestart;
    }

    /// Returns true if the device supports VK_EXT_primitive_topology_list_restart.
    bool IsPatchListPrimitiveRestartSupported() const {
        return features.primitive_topology_list_restart.primitiveTopologyPatchListRestart;
    }

    /// Returns true if the device supports VK_EXT_index_type_uint8.
    bool IsExtIndexTypeUint8Supported() const {
        return extensions.index_type_uint8;
    }

    /// Returns true if the device supports VK_EXT_sampler_filter_minmax.
    bool IsExtSamplerFilterMinmaxSupported() const {
        return extensions.sampler_filter_minmax;
    }

    /// Returns true if the device supports VK_EXT_shader_stencil_export.
    bool IsExtShaderStencilExportSupported() const {
        return extensions.shader_stencil_export;
    }

    /// Returns true if the device supports VK_KHR_depth_stencil_resolve.
    bool IsKhrDepthStencilResolveSupported() const {
        return extensions.depth_stencil_resolve || instance_version >= VK_API_VERSION_1_2;
    }

    /// Returns the supported resolve modes for the depth aspect.
    VkResolveModeFlags GetDepthResolveModes() const {
        return properties.depth_stencil_resolve.supportedDepthResolveModes;
    }

    /// Returns the supported resolve modes for the stencil aspect.
    VkResolveModeFlags GetStencilResolveModes() const {
        return properties.depth_stencil_resolve.supportedStencilResolveModes;
    }

    /// Returns true if only one of the depth and stencil aspects may be resolved.
    bool SupportsIndependentResolveNone() const {
        return properties.depth_stencil_resolve.independentResolveNone == VK_TRUE;
    }

    /// Returns true if the device supports VK_EXT_depth_range_unrestricted.
    bool IsExtDepthRangeUnrestrictedSupported() const {
        return extensions.depth_range_unrestricted;
    }

    /// Returns true if the device supports VK_EXT_depth_clip_control.
    bool IsExtDepthClipControlSupported() const {
        return extensions.depth_clip_control;
    }

    bool IsLoadOpNoneSupported() const {
        return extensions.load_store_op_none || extensions.load_store_op_none_ext;
    }

    bool IsExtDynamicRenderingUnusedAttachmentsSupported() const {
        return extensions.dynamic_rendering_unused_attachments;
    }

    /// Returns true if the device supports VK_EXT_depth_bias_control.
    bool IsExtDepthBiasControlSupported() const {
        return extensions.depth_bias_control;
    }

    /// Returns true if the device supports VK_EXT_shader_viewport_index_layer.
    bool IsExtShaderViewportIndexLayerSupported() const {
        return extensions.shader_viewport_index_layer;
    }

    /// Returns true if the device supports VK_EXT_subgroup_size_control.
    bool IsExtSubgroupSizeControlSupported() const {
        return extensions.subgroup_size_control;
    }

    /// Returns true if vkResetQueryPool (host-side query reset) is supported.
    bool IsHostQueryResetSupported() const {
        return features.host_query_reset.hostQueryReset != VK_FALSE;
    }

    u32 GetMaxVertexAttribDivisor() const {
        const u32 reported = properties.vertex_attribute_divisor.maxVertexAttribDivisor;
        if (reported == 0) {
            return 1;
        }
        return reported;
    }

    bool IsVertexAttributeInstanceRateZeroDivisorSupported() const {
        return features.vertex_attribute_divisor.vertexAttributeInstanceRateZeroDivisor == VK_TRUE;
    }

    u32 GetVertexAttribDivisor(u32 frequency) const {
        const u32 max_divisor = GetMaxVertexAttribDivisor();
        if (frequency == 0) {
            if (IsVertexAttributeInstanceRateZeroDivisorSupported()) {
                return 0;
            }
            return max_divisor;
        }
        if (frequency > max_divisor) {
            return max_divisor;
        }
        return frequency;
    }

    /// Returns true if the device supports VK_EXT_transform_feedback.
    bool IsExtTransformFeedbackSupported() const {
        return extensions.transform_feedback;
    }

    /// Returns true if transform feedback draw commands are supported.
    bool IsTransformFeedbackDrawSupported() const {
        return extensions.transform_feedback && properties.transform_feedback.transformFeedbackDraw;
    }

    /// Returns true if the device supports VK_EXT_transform_feedback properly.
    bool AreTransformFeedbackGeometryStreamsSupported() const {
        return features.transform_feedback.geometryStreams;
    }

    /// Returns true if custom border colors can be created without a format.
    bool IsCustomBorderColorUsable() const {
        return extensions.custom_border_color &&
               features.custom_border_color.customBorderColors &&
               features.custom_border_color.customBorderColorWithoutFormat;
    }

    /// Takes budget for samplers carrying a custom border color, false when exhausted.
    bool TryReserveCustomBorderColorSamplers(size_t count) const;

    /// Gives back budget taken by TryReserveCustomBorderColorSamplers.
    void ReleaseCustomBorderColorSamplers(size_t count) const;

    /// Returns true if the device supports VK_EXT_color_write_enable.
    bool IsExtColorWriteEnableSupported() const {
        return extensions.color_write_enable;
    }

    /// Returns true if the device supports VK_EXT_border_color_swizzle.
    bool IsExtBorderColorSwizzleSupported() const {
        return extensions.border_color_swizzle;
    }

    /// Returns true if samplers must be carried with border color swizzle mapping.
    bool NeedsBorderColorSwizzleMapping() const {
        return extensions.border_color_swizzle &&
               !features.border_color_swizzle.borderColorSwizzleFromImage;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state.
    bool IsExtExtendedDynamicStateSupported() const {
        return extensions.extended_dynamic_state;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state2.
    bool IsExtExtendedDynamicState2Supported() const {
        return extensions.extended_dynamic_state2;
    }

    bool IsExtExtendedDynamicState2ExtrasSupported() const {
        return features.extended_dynamic_state2.extendedDynamicState2LogicOp;
    }

    /// Returns true if the device supports VK_EXT_4444_formats.
    bool IsExt4444FormatsSupported() const {
        return features.format_a4b4g4r4.formatA4B4G4R4;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state3.
    bool IsExtExtendedDynamicState3BlendingSupported() const {
        return dynamic_state3_blending;
    }

    /// Returns true if the device supports VK_EXT_extended_dynamic_state3.
    bool IsExtExtendedDynamicState3EnablesSupported() const {
        return dynamic_state3_enables;
    }

    /// Returns true if the device supports VK_EXT_filter_cubic
    bool IsExtFilterCubicSupported() const {
        return extensions.filter_cubic;
    }

    /// Returns true if the device supports VK_QCOM_filter_cubic_weights
    bool IsQcomFilterCubicWeightsSupported() const {
        return extensions.filter_cubic_weights;
    }

    /// Returns true if the device supports VK_EXT_line_rasterization.
    bool IsExtLineRasterizationSupported() const {
        return extensions.line_rasterization;
    }

    bool SupportsRectangularLines() const {
        return features.line_rasterization.rectangularLines != VK_FALSE;
    }

    bool SupportsSmoothLines() const {
        return features.line_rasterization.smoothLines != VK_FALSE;
    }

    bool SupportsStippledRectangularLines() const {
        return features.line_rasterization.stippledRectangularLines != VK_FALSE;
    }

    VkLineRasterizationModeEXT GetLineRasterizationMode(bool wants_smooth) const {
        if (wants_smooth && SupportsSmoothLines()) {
            return VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        }
        if (SupportsRectangularLines()) {
            return VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
        }
        return VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
    }

    bool SupportsStippleForMode(VkLineRasterizationModeEXT mode) const {
        switch (mode) {
        case VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT:
            return features.line_rasterization.stippledSmoothLines != VK_FALSE;
        case VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT:
            return features.line_rasterization.stippledBresenhamLines != VK_FALSE;
        default:
            return features.line_rasterization.stippledRectangularLines != VK_FALSE;
        }
    }

    float ClampLineWidth(float width) const {
        if (!features.features.wideLines) {
            return 1.0f;
        }
        const auto& range = properties.properties.limits.lineWidthRange;
        if (!(width >= range[0])) {
            return range[0];
        }
        return width > range[1] ? range[1] : width;
    }

    bool SupportsAlphaToOne() const {
        return features.features.alphaToOne != VK_FALSE;
    }

    bool SupportsDynamicState3DepthClampEnable() const {
        return dynamic_state3_depth_clamp_enable;
    }

    bool SupportsDynamicState3LogicOpEnable() const {
        return dynamic_state3_logic_op_enable;
    }

    bool SupportsDynamicState3LineRasterizationMode() const {
        return dynamic_state3_line_raster_mode;
    }

    bool SupportsDynamicState3ConservativeRasterizationMode() const {
        return dynamic_state3_conservative_raster_mode;
    }

    bool SupportsDynamicState3LineStippleEnable() const {
        return dynamic_state3_line_stipple_enable;
    }

    bool SupportsDynamicState3AlphaToCoverageEnable() const {
        return dynamic_state3_alpha_to_coverage;
    }

    bool SupportsDynamicState3AlphaToOneEnable() const {
        return dynamic_state3_alpha_to_one;
    }

    /// Returns true if the device supports VK_EXT_vertex_input_dynamic_state.
    bool IsExtVertexInputDynamicStateSupported() const {
        return extensions.vertex_input_dynamic_state;
    }

    /// Returns true if the device supports VK_EXT_shader_demote_to_helper_invocation
    bool IsExtShaderDemoteToHelperInvocationSupported() const {
        return extensions.shader_demote_to_helper_invocation;
    }

    /// Returns true if the device supports VK_EXT_conservative_rasterization.
    bool IsExtConservativeRasterizationSupported() const {
        return extensions.conservative_rasterization;
    }

    /// Returns true if the device supports VK_EXT_provoking_vertex.
    bool IsExtProvokingVertexSupported() const {
        return extensions.provoking_vertex;
    }

    /// Returns true if first vertex provoking mode can be used.
    bool SupportsProvokingVertexFirstMode() const {
        return extensions.provoking_vertex;
    }

    /// Returns true if last vertex provoking mode can be used.
    bool SupportsProvokingVertexLastMode() const {
        return extensions.provoking_vertex && features.provoking_vertex.provokingVertexLast;
    }

    /// Returns true if transform feedback preserves provoking vertex mode semantics.
    bool SupportsTransformFeedbackProvokingVertexPreservation() const {
        return extensions.provoking_vertex &&
               features.provoking_vertex.transformFeedbackPreservesProvokingVertex;
    }

    /// Returns true if the device supports int64 atomics on storage buffers.
    bool IsExtShaderAtomicInt64Supported() const {
        return extensions.shader_atomic_int64 &&
               features.shader_atomic_int64.shaderBufferInt64Atomics;
    }

    /// Returns true if the device supports int64 atomics on workgroup (shared) memory.
    bool IsSharedInt64AtomicsSupported() const {
        return extensions.shader_atomic_int64 &&
               features.shader_atomic_int64.shaderSharedInt64Atomics;
    }

    bool IsSharedInt64AtomicsSupported() const {
        return extensions.shader_atomic_int64 &&
               features.shader_atomic_int64.shaderSharedInt64Atomics;
    }

    /// Returns true if the device supports VK_EXT_conditional_rendering.
    bool IsExtConditionalRendering() const {
        return extensions.conditional_rendering;
    }

    VkPipelineStageFlags2 GetBufferUserStages() const {
        VkPipelineStageFlags2 stages = vk::PIPELINE_STAGE_BUFFER_USERS;
        if (IsExtConditionalRendering()) {
            stages |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
        }
        if (IsExtTransformFeedbackSupported()) {
            stages |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        }
        return stages;
    }

    VkPipelineStageFlags2 GetBufferConsumerStages() const {
        return GetBufferUserStages() | VK_PIPELINE_STAGE_2_HOST_BIT;
    }

    VkAccessFlags2 GetBufferConsumerAccess() const {
        VkAccessFlags2 access = vk::ACCESS_BUFFER_CONSUMERS;
        if (IsExtConditionalRendering()) {
            access |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
        }
        if (IsExtTransformFeedbackSupported()) {
            access |= VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT |
                      VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
                      VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        }
        return access;
    }

    bool IsExtAstcDecodeModeSupported() const {
        return extensions.astc_decode_mode;
    }

    /// Returns true if the device supports VK_EXT_external_memory_host.
    bool IsExtExternalMemoryHostSupported() const {
        return extensions.external_memory_host;
    }

    bool IsExtExternalMemoryAhbSupported() const {
#ifdef __ANDROID__
        return extensions.external_memory_ahb && extensions.queue_family_foreign;
#else
        return false;
#endif
    }

    bool IsDescriptorBindingPartiallyBoundSupported() const {
        return features.descriptor_indexing.descriptorBindingPartiallyBound;
    }

    u64 GetMinImportedHostPointerAlignment() const {
        return properties.external_memory_host.minImportedHostPointerAlignment;
    }

    u64 GetMaxBufferSize() const {
        return properties.maintenance4.maxBufferSize;
    }

    u64 GetMaxMemoryAllocationSize() const {
        return properties.maintenance3.maxMemoryAllocationSize;
    }

    bool HasTimelineSemaphore() const;

    /// Returns the minimum supported version of SPIR-V.
    u32 SupportedSpirvVersion() const {
        if (instance_version >= VK_API_VERSION_1_3) {
            return 0x00010600U;
        }
        if (extensions.spirv_1_4) {
            return 0x00010400U;
        }
        return 0x00010300U;
    }

    /// Returns true for an integrated GPU (its device-local memory is system memory).
    bool IsIntegrated() const {
        return is_integrated;
    }

    /// Returns true when a known debugging tool is attached.
    bool HasDebuggingToolAttached() const {
        return has_renderdoc || has_nsight_graphics || has_radeon_gpu_profiler;
    }

    /// @returns True if compute pipelines can cause crashing.
    bool HasBrokenCompute() const {
        return has_broken_compute;
    }

    /// Returns true when the device does not properly support cube compatibility.
    bool HasBrokenCubeImageCompatibility() const {
        return has_broken_cube_compatibility;
    }

    /// Returns true if parallel shader compiling has issues with the current driver.
    bool HasBrokenParallelShaderCompiling() const {
        return has_broken_parallel_compiling;
    }

    /// Returns the vendor name reported from Vulkan.
    std::string_view GetVendorName() const {
        return properties.driver.driverName;
    }

    /// Returns the list of available extensions.
    const std::set<std::string, std::less<>>& GetAvailableExtensions() const {
        return supported_extensions;
    }

    u64 GetDeviceLocalMemory() const {
        return device_access_memory;
    }

    bool CanReportMemoryUsage() const {
        return extensions.memory_budget;
    }

    u64 GetDeviceMemoryUsage() const;

    VkSampleCountFlags GetStorageImageSampleCounts() const {
        return properties.properties.limits.storageImageSampleCounts;
    }

    u32 GetSetsPerPool() const {
        return sets_per_pool;
    }

    bool SupportsD24DepthBuffer() const {
        return supports_d24_depth;
    }

    bool MustEmulateScaledFormats() const {
        return must_emulate_scaled_formats;
    }

    bool IsUMA() const {
        return is_uma;
    }

    bool IsHostImageCopySupported() const {
        return extensions.host_image_copy;
    }

    bool HasNullDescriptor() const {
        return features.robustness2.nullDescriptor;
    }

    bool MustEmulateBGR565() const;

    u32 GetMaxVertexInputAttributes() const {
        return properties.properties.limits.maxVertexInputAttributes;
    }

    u32 GetMaxVertexInputBindings() const {
        return properties.properties.limits.maxVertexInputBindings;
    }

    u32 GetMaxVertexAttribDivisor() const {
        return max_vertex_attrib_divisor;
    }

    u32 GetMaxViewports() const {
        return properties.properties.limits.maxViewports;
    }

    u32 GetMaxUserClipDistances() const {
        return properties.properties.limits.maxClipDistances;
    }

    bool SupportsConditionalBarriers() const {
        return supports_conditional_barriers;
    }

    bool SupportsMultiViewport() const {
        return features2.features.multiViewport;
    }

    /// Returns true if the device supports VK_KHR_maintenance4.
    bool IsKhrMaintenance4Supported() const {
        return extensions.maintenance4;
    }

    /// Returns true if the device can read the draw count from a buffer.
    bool IsDrawIndirectCountSupported() const {
        return extensions.draw_indirect_count;
    }

    /// Returns true if the device supports VK_EXT_vertex_attribute_divisor.
    bool IsExtVertexAttributeDivisorSupported() const {
        return extensions.vertex_attribute_divisor;
    }

    /// Returns true if the device supports VK_KHR_sampler_mirror_clamp_to_edge.
    bool IsKhrSamplerMirrorClampToEdgeSupported() const {
        return extensions.sampler_mirror_clamp_to_edge;
    }

    bool IsKhrMaintenance5Supported() const {
        return extensions.maintenance5;
    }

    /// Returns true if depth/stencil swizzle ONE is supported.
    bool SupportsDepthStencilSwizzleOne() const {
        return extensions.maintenance5 && properties.maintenance5.depthStencilSwizzleOneSupport;
    }

    /// Returns true if the device supports UINT8 index buffer conversion via compute shader.
    bool SupportsUint8Indices() const {
        return features.bit8_storage.storageBuffer8BitAccess &&
               features.bit16_storage.storageBuffer16BitAccess;
    }

    /// Returns true if the device supports reading 8-bit values from a storage buffer.
    bool IsStorageBuffer8BitAccessSupported() const {
        return features.bit8_storage.storageBuffer8BitAccess;
    }

    /// Returns true if the device supports reading 16-bit values from a storage buffer.
    bool IsStorageBuffer16BitAccessSupported() const {
        return features.bit16_storage.storageBuffer16BitAccess;
    }

    [[nodiscard]] static constexpr bool CheckBrokenCompute(VkDriverId driver_id,
                                                           u32 driver_version) {
        if (driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
            const u32 major = VK_API_VERSION_MAJOR(driver_version);
            const u32 minor = VK_API_VERSION_MINOR(driver_version);
            const u32 patch = VK_API_VERSION_PATCH(driver_version);
            if (major == 0 && minor == 405 && patch < 286) {
                LOG_WARNING(
                    Render_Vulkan,
                    "Intel proprietary drivers 0.405.0 until 0.405.286 have broken compute");
                return true;
            }
        }
        return false;
    }

    bool IsNvidia() const noexcept {
        return properties.driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    }

    bool IsMoltenVK() const noexcept {
        return properties.driver.driverID == VK_DRIVER_ID_MOLTENVK;
    }

    bool IsQualcomm() const noexcept {
        return properties.driver.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    }

    NvidiaArchitecture GetNvidiaArch() const noexcept {
        return nvidia_arch;
    }

    /// GPU logging integration
    void InitializeGPULogging();
    void ShutdownGPULogging();

private:
    /// Picks a usable resolve mode out of a supported-modes mask. SAMPLE_ZERO is preferred because
    /// it is the only mode the spec guarantees whenever any depth/stencil resolve is supported,
    /// and averaging depth samples is not meaningful for depth testing anyway.
    static VkResolveModeFlagBits PickResolveMode(VkResolveModeFlags supported) {
        if ((supported & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0) {
            return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        }
        if ((supported & VK_RESOLVE_MODE_MIN_BIT) != 0) {
            return VK_RESOLVE_MODE_MIN_BIT;
        }
        if ((supported & VK_RESOLVE_MODE_MAX_BIT) != 0) {
            return VK_RESOLVE_MODE_MAX_BIT;
        }
        return VK_RESOLVE_MODE_NONE;
    }

    /// Checks if the physical device is suitable and configures the object state
    /// with all necessary info about its properties.
    bool GetSuitability(bool requires_swapchain);

    // Remove extensions which have incomplete feature support.
    void RemoveUnsuitableExtensions();

    void RemoveExtension(bool& extension, const std::string& extension_name);
    void RemoveExtensionIfUnsuitable(bool is_suitable, const std::string& extension_name);

    template <typename Feature>
    void RemoveExtensionFeature(bool& extension, Feature& feature,
                                const std::string& extension_name);
    template <typename Feature>
    void RemoveExtensionFeatureIfUnsuitable(bool is_suitable, Feature& feature,
                                            const std::string& extension_name);

    /// Sets up queue families.
    void SetupFamilies(VkSurfaceKHR surface);

    /// Collects information about attached tools.
    void CollectToolingInfo();

    /// Collects information about the device's local memory.
    void CollectPhysicalMemoryInfo();

    /// Returns a list of queue initialization descriptors.
    std::vector<VkDeviceQueueCreateInfo> GetDeviceQueueCreateInfos() const;

    /// Returns true if ASTC textures are natively supported.
    bool ComputeIsOptimalAstcSupported() const;

    /// Returns true if the device natively supports blitting depth stencil images.
    bool TestDepthStencilBlits(VkFormat format) const;

private:
    VkInstance instance;         ///< Vulkan instance.
    VmaAllocator allocator;      ///< VMA allocator.
    mutable u32 allocator_frame_index{};
    vk::DeviceDispatch dld;      ///< Device function pointers.
    vk::PhysicalDevice physical; ///< Physical device.
    vk::Device logical;          ///< Logical device.
    vk::Queue graphics_queue;    ///< Main graphics queue.
    vk::Queue present_queue;     ///< Main present queue.
    u32 instance_version{};      ///< Vulkan instance version.
    u32 graphics_family{};       ///< Main graphics queue family index.
    u32 present_family{};        ///< Main present queue family index.
    bool graphics_family_sparse_binding{};

    struct Extensions {
#define EXTENSION(prefix, macro_name, var_name) bool var_name{};
#define FEATURE(prefix, struct_name, macro_name, var_name) bool var_name{};
        FOR_EACH_VK_FEATURE_1_1(FEATURE);
        FOR_EACH_VK_FEATURE_1_2(FEATURE);
        FOR_EACH_VK_FEATURE_1_3(FEATURE);
        FOR_EACH_VK_FEATURE_1_4(FEATURE);
        FOR_EACH_VK_FEATURE_EXT(FEATURE);
        FOR_EACH_VK_EXTENSION(EXTENSION);
        FOR_EACH_VK_PLATFORM_EXTENSION(EXTENSION);

#undef EXTENSION
#undef FEATURE
    };

    struct Features {
#define FEATURE_CORE(prefix, struct_name, macro_name, var_name) VkPhysicalDevice##struct_name##Features var_name{};
#define FEATURE_EXT(prefix, struct_name, macro_name, var_name) VkPhysicalDevice##struct_name##Features##prefix var_name{};
        FOR_EACH_VK_FEATURE_1_1(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_2(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_3(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_1_4(FEATURE_CORE);
        FOR_EACH_VK_FEATURE_EXT(FEATURE_EXT);
#undef FEATURE_CORE
#undef FEATURE_EXT
        VkPhysicalDeviceFaultFeaturesEXT device_fault{};
        VkPhysicalDeviceFeatures features{};
    };

    struct Properties {
        VkPhysicalDeviceDriverProperties driver{};
        VkPhysicalDeviceSubgroupProperties subgroup_properties{};
        VkPhysicalDeviceFloatControlsProperties float_controls{};
        VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor{};
        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer{};
        VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_control{};
        VkPhysicalDeviceTexelBufferAlignmentProperties texel_buffer_alignment{};
        VkPhysicalDeviceTransformFeedbackPropertiesEXT transform_feedback{};
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vertex_attribute_divisor{};
        VkPhysicalDeviceMaintenance5PropertiesKHR maintenance5{};
        VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve{};
        VkPhysicalDeviceCustomBorderColorPropertiesEXT custom_border_color{};
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vertex_attribute_divisor{};

        VkPhysicalDeviceProperties properties{};
    };

    Extensions extensions{};
    Features features{};
    Properties properties{};
    u32 max_vertex_attrib_divisor{1};

    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceProperties2 properties2{};

    // Misc features
    bool is_optimal_astc_supported{};          ///< Support for all guest ASTC formats.
    bool is_blit_depth24_stencil8_supported{}; ///< Support for blitting from and to D24S8.
    bool is_blit_depth32_stencil8_supported{}; ///< Support for blitting from and to D32S8.
    bool is_warp_potentially_bigger{};         ///< Host warp size can be bigger than guest.
    bool is_integrated{};                      ///< Is GPU an iGPU.
    bool is_uma{};
    bool has_broken_compute{};                 ///< Compute shaders can cause crashes
    bool has_broken_cube_compatibility{};      ///< Has broken cube compatibility bit
    bool has_broken_float16_math{};
    bool has_broken_descriptor_aliasing{};     ///< Miscompiles descriptors aliased on one binding
    bool has_broken_parallel_compiling{};      ///< Has broken parallel shader compiling.
    bool has_renderdoc{};                      ///< Has RenderDoc attached
    bool has_nsight_graphics{};                ///< Has Nsight Graphics attached
    bool has_radeon_gpu_profiler{};            ///< Has Radeon GPU Profiler attached.
    bool supports_d24_depth{};                 ///< Supports D24 depth buffers.
    bool must_emulate_scaled_formats{};        ///< Requires scaled vertex format emulation
    bool dynamic_state3_blending{};            ///< Has blending features of dynamic_state3.
    bool dynamic_state3_enables{};             ///< Has at least one enable feature of dynamic_state3.
    bool dynamic_state3_depth_clamp_enable{};
    bool dynamic_state3_logic_op_enable{};
    bool dynamic_state3_line_raster_mode{};
    bool dynamic_state3_conservative_raster_mode{};
    bool dynamic_state3_line_stipple_enable{};
    bool dynamic_state3_alpha_to_coverage{};
    bool dynamic_state3_alpha_to_one{};
    bool supports_conditional_barriers{};      ///< Allows barriers in conditional control flow.
    mutable std::atomic<size_t> custom_border_color_samplers_used{};
    u64 device_access_memory{};                ///< Total size of device local memory in bytes.
    u32 sets_per_pool{};                       ///< Sets per Description Pool
    NvidiaArchitecture nvidia_arch{NvidiaArchitecture::Arch_AmpereOrNewer};

    // Telemetry parameters
    std::set<std::string, std::less<>> supported_extensions; ///< Reported Vulkan extensions.
    std::set<std::string, std::less<>> loaded_extensions;    ///< Loaded Vulkan extensions.
    bool streamline_extensions{}; ///< NVIDIA DLSS's extensions are loaded.
    bool streamline_attached{};   ///< Streamline has this device.
    std::vector<size_t> valid_heap_memory;                   ///< Heaps used.
    size_t main_heap{};       ///< Largest device-local heap, where images live.
    u64 heap_size_limit{};    ///< EDEN_NX2_VRAM_LIMIT_MIB for device-local heaps, 0 if unset.

    /// Format properties dictionary.
    ::Common::unordered_map<VkFormat, VkFormatProperties> format_properties;

    /// Nsight Aftermath GPU crash tracker
    std::unique_ptr<NsightAftermathTracker> nsight_aftermath_tracker;
};

} // namespace Vulkan
