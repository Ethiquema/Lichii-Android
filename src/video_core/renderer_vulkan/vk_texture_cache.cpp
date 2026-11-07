// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <array>
#include <optional>
#include <span>
#include <memory>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <bit>
#include <numeric>
#include "common/alignment.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "common/settings.h"

#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/nx2.h"
#include "video_core/gpu_logging/gpu_logging.h"

#include "video_core/engines/fermi_2d.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/textures/decoders.h"

namespace Vulkan {

using Tegra::Engines::Fermi2D;
using Tegra::Texture::SwizzleSource;
using Tegra::Texture::TextureMipmapFilter;
using VideoCommon::BufferImageCopy;
using VideoCommon::ImageFlagBits;
using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCommon::SubresourceRange;
using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::HasAlpha;
using VideoCore::Surface::IsPixelFormatASTC;
using VideoCore::Surface::IsPixelFormatInteger;
using VideoCore::Surface::IsPixelFormatSRGB;
using VideoCore::Surface::SurfaceType;

namespace {
constexpr bool ENABLE_MSAA_TILER_RESOLVE = true;
constexpr bool ENABLE_MSAA_RESOLVE_CONSUME = true;
constexpr bool ENABLE_MSAA_COLOR_DISCARD = true;
constexpr bool ENABLE_MSAA_DEPTH_STENCIL_DISCARD = true;

[[nodiscard]] constexpr bool NeedsExplicitBorderColorFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        return true;
    default:
        return false;
    }
}

constexpr VkBorderColor ConvertBorderColor(const std::array<float, 4>& color) {
    if (color == std::array<float, 4>{0, 0, 0, 0}) {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    } else if (color == std::array<float, 4>{0, 0, 0, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else if (color == std::array<float, 4>{1, 1, 1, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    if (color[0] + color[1] + color[2] > 1.35f) {
        // If color elements are brighter than roughly 0.5 average, use white border
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (color[3] > 0.5f) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}

[[nodiscard]] VkImageType ConvertImageType(const ImageType type) {
    switch (type) {
    case ImageType::e1D:
        return VK_IMAGE_TYPE_1D;
    case ImageType::e2D:
    case ImageType::Linear:
        return VK_IMAGE_TYPE_2D;
    case ImageType::e3D:
        return VK_IMAGE_TYPE_3D;
    case ImageType::Buffer:
        break;
    }
    ASSERT_MSG(false, "Invalid image type={}", type);
    return {};
}

[[nodiscard]] VkSampleCountFlagBits ConvertSampleCount(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        ASSERT_MSG(false, "Invalid number of samples={}", num_samples);
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

[[nodiscard]] VkImageUsageFlags ImageUsageFlags(const MaxwellToVK::FormatInfo& info,
                                                PixelFormat format, bool allow_storage = true) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info.attachable) {
        switch (VideoCore::Surface::GetFormatType(format)) {
        case VideoCore::Surface::SurfaceType::ColorTexture:
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            break;
        case VideoCore::Surface::SurfaceType::Depth:
        case VideoCore::Surface::SurfaceType::Stencil:
        case VideoCore::Surface::SurfaceType::DepthStencil:
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            break;
        default:
            ASSERT_MSG(false, "Invalid surface type");
            break;
        }
    }
    if (info.storage && allow_storage) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (IsPixelFormatASTC(format)) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    return usage;
}

// An ASTC 2D array whose RGBA8 decode exceeds 1 GiB (title 1's 8192x8192x68 terrain textures).
// These stay on the GPU decoder as RGBA8 with trimmed levels (TrimmedHostInfo) even when other
// ASTC images are re-compressed to BC1/BC3 on the CPU: re-compressing them would not fit either.
[[nodiscard]] bool IsHugeAstcArray(const Device& device, const ImageInfo& info) {
    if (!IsPixelFormatASTC(info.format) || device.IsOptimalAstcSupported() ||
        info.type != ImageType::e2D || info.size.depth != 1) {
        return false;
    }
    u64 bytes = 0;
    for (s32 level = 0; level < info.resources.levels; ++level) {
        bytes += u64{(std::max)(info.size.width >> level, 1U)} *
                 (std::max)(info.size.height >> level, 1U) * 4;
    }
    return bytes * info.resources.layers > (u64{1} << 30);
}

// A large image over Switch 2 sparse blocks nobody has written (title 1's terrain layer pools, 2.9
// GB of R8/RG16/R16 that stay empty in the attract demo) is all zeros: it gets no device memory
// until a write makes the texture cache replace it.
[[nodiscard]] bool IsUnwrittenSparse(const Device& device, const ImageInfo& info, GPUVAddr gpu_addr) {
    if (!device.CanUseUnbackedSparseImages()) {
        return false;
    }
    constexpr u32 MinSize = 32U << 20;
    const u32 size = VideoCommon::CalculateGuestSizeInBytes(info);
    return size >= MinSize && info.type != ImageType::e1D && info.type != ImageType::Buffer &&
           info.type != ImageType::Linear && Tegra::Nx2SparseUnwritten(gpu_addr, size);
}

// A large color array in a Switch 2 sparse region (title 1's terrain arrays: 13-26% of their
// blocks written) gets memory by layer, as its layers are written: unbacked at first, then
// Image::BindLayers. Only formats uploaded layer by layer: huge ASTC arrays (GPU decode) and
// formats the host samples as they are.
[[nodiscard]] bool UsesSparseLayers(const Device& device, const ImageInfo& info, GPUVAddr gpu_addr) {
    if (!device.CanUseUnbackedSparseImages() || !device.IsSparseResidencyImage2DSupported() ||
        info.type != ImageType::e2D ||
        info.resources.layers <= 1 || info.num_samples != 1 ||
        VideoCore::Surface::GetFormatType(info.format) != VideoCore::Surface::SurfaceType::ColorTexture) {
        return false;
    }
    const bool layer_uploads =
        IsHugeAstcArray(device, info) ||
        (!IsPixelFormatASTC(info.format) &&
         (!IsPixelFormatBCn(info.format) || device.IsOptimalBcnSupported()));
    const u32 size = VideoCommon::CalculateGuestSizeInBytes(info);
    return layer_uploads && size >= (32U << 20) && Tegra::Nx2InSparseRegion(gpu_addr, size);
}

// How SparseTiles images leave the blocks without data: unbound (reads give zero), or all bound to
// one zeroed block where the driver can't sample unbound memory (AMD) but lets blocks share
// memory. EDEN_NX2_SPARSE_ALIASED=1 takes the shared block wherever it is supported, =0 never.
enum class SparseTilesMode {
    None,
    Unbound,
    Aliased,
};

[[nodiscard]] SparseTilesMode SparseTilesModeOf(const Device& device) {
    static const char force_aliased = [] {
        const char* value = std::getenv("EDEN_NX2_SPARSE_ALIASED");
        return value == nullptr ? '\0' : value[0];
    }();
    if (!device.IsSparseResidencyImage2DSupported()) {
        return SparseTilesMode::None;
    }
    const bool aliased = device.IsSparseResidencyAliasedSupported() && force_aliased != '0';
    if (aliased && force_aliased == '1') {
        return SparseTilesMode::Aliased;
    }
    if (device.CanUseUnbackedSparseImages()) {
        return SparseTilesMode::Unbound;
    }
    return aliased ? SparseTilesMode::Aliased : SparseTilesMode::None;
}

// A large single 2D image in a Switch 2 sparse region (title 1's world-sized R8/RG16/R16 masks,
// 170-341 MB, of which the game maps a few MiB) gets memory by sparse block where the guest memory
// under it can hold data: unbacked at first, then Image::BindTiles. Formats of one texel per
// block that the host samples as they are.
[[nodiscard]] bool UsesSparseTiles(const Device& device, const ImageInfo& info, GPUVAddr gpu_addr) {
    if (SparseTilesModeOf(device) == SparseTilesMode::None || info.type != ImageType::e2D || info.resources.layers != 1 || info.num_samples != 1 ||
        info.size.depth != 1 || info.block.width != 0 ||
        VideoCore::Surface::DefaultBlockWidth(info.format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(info.format) != 1 ||
        VideoCore::Surface::GetFormatType(info.format) != VideoCore::Surface::SurfaceType::ColorTexture) {
        return false;
    }
    const u32 size = VideoCommon::CalculateGuestSizeInBytes(info);
    return size >= (32U << 20) && Tegra::Nx2InSparseRegion(gpu_addr, size);
}

[[nodiscard]] bool IsUnswizzleAcceleratedFormat(const Device& device, PixelFormat format) {
    if (!ENABLE_ACCELERATED_UNSWIZZLE) {
        return false;
    }
    if (DefaultBlockWidth(format) != 1 || DefaultBlockHeight(format) != 1) {
        return false;
    }
    return VideoCore::Surface::IsPixelFormatSRGB(info.format) ? VK_FORMAT_A8B8G8R8_SRGB_PACK32
                                                              : VK_FORMAT_A8B8G8R8_UNORM_PACK32;
}

constexpr std::array<VkFormat, 2> HugeAstcViewFormats{VK_FORMAT_A8B8G8R8_UNORM_PACK32,
                                                       VK_FORMAT_A8B8G8R8_SRGB_PACK32};

// Opaque ASTC (ImageInfo::astc_opaque) re-compressed to BC1 instead of BC3.
[[nodiscard]] bool IsOpaqueBc1Astc(const Device& device, const ImageInfo& info) {
    return info.astc_opaque && IsPixelFormatASTC(info.format) && !IsHugeAstcArray(device, info);
}

constexpr std::array<VkFormat, 2> OpaqueAstcViewFormats{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                                         VK_FORMAT_BC1_RGBA_SRGB_BLOCK};

[[nodiscard]] std::optional<VkFormat> AstcHostFormat(const Device& device, const ImageInfo& info) {
    if (IsOpaqueBc1Astc(device, info)) {
        return VideoCore::Surface::IsPixelFormatSRGB(info.format) ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK
                                                                  : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    }
    if (!MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, format).storage) {
        return false;
    }
    return IsUnswizzleStorageFormatSupported(device, BytesPerBlock(format));
}

[[nodiscard]] bool WillUseAcceleratedUnswizzle(const Device& device, const ImageInfo& info) {
    const bool supported_type = info.type == ImageType::e2D || info.type == ImageType::e3D ||
                                info.type == ImageType::Linear;
    if (!supported_type || info.num_samples > 1) {
        return false;
    }
    switch (info.type) {
    case ImageType::e2D:
    case ImageType::e3D:
    case ImageType::Linear:
        break;
    default:
        return false;
    }
    if (info.type != ImageType::e2D && info.type != ImageType::e3D &&
        info.type != ImageType::Linear) {
        return false;
    }
    if (IsHugeAstcArray(device, info)) {
        return true;
    }
    if (Settings::values.accelerate_astc.GetValue() != Settings::AstcDecodeMode::Gpu) {
        return false;
    }
    return info.size.depth == 1;
}

// The most a trimmed ASTC array keeps on the host: 512 MiB, 256 MiB on GPUs with less than 7 GiB
// (title 1's two terrain arrays took 724 MiB of a 6 GB card next to ~1.2 GB of other sparse terrain
// textures). EDEN_NX2_ASTC_ARRAY_MIB=<MiB> sets it.
[[nodiscard]] u64 TrimmedAstcArrayLimit(const Device& device) {
    static const u64 forced = [] {
        const char* value = std::getenv("EDEN_NX2_ASTC_ARRAY_MIB");
        return value != nullptr ? std::strtoull(value, nullptr, 10) << 20 : 0ULL;
    }();
    if (forced != 0) {
        return forced;
    }
    return device.GetDeviceLocalMemory() < (u64{7} << 30) ? (u64{256} << 20) : (u64{512} << 20);
}

// The host shape of an image whose decoded copy would not fit in VRAM: a GPU-decoded ASTC array
// over 1 GiB as RGBA8 (title 1's 8192x8192x68 ASTC 8x8 virtual textures are 24 GB each) keeps only
// its mip levels from `level_offset` on, so host mip 0 is guest mip `level_offset`. Normalized
// coordinates and derivative LOD still address it correctly; explicit LODs sample one level lower.
[[nodiscard]] ImageInfo TrimmedHostInfo(const Device& device, const ImageInfo& info, u32& level_offset) {
    level_offset = 0;
    if (!WillUseAcceleratedAstcDecode(device, info) || info.type != ImageType::e2D) {
        return info;
    }
    const auto decoded_bytes = [&](u32 skip) {
        u64 bytes = 0;
        for (s32 level = static_cast<s32>(skip); level < info.resources.levels; ++level) {
            bytes += u64{(std::max)(info.size.width >> level, 1U)} *
                     (std::max)(info.size.height >> level, 1U) * 4;
        }
        return bytes * info.resources.layers;
    };
    if (decoded_bytes(0) <= (u64{1} << 30)) {
        return info;
    }
    while (level_offset + 1 < static_cast<u32>(info.resources.levels) &&
           decoded_bytes(level_offset) > (u64{512} << 20)) {
        ++level_offset;
    }
    ImageInfo host = info;
    host.size.width = (std::max)(info.size.width >> level_offset, 1U);
    host.size.height = (std::max)(info.size.height >> level_offset, 1U);
    host.resources.levels -= static_cast<s32>(level_offset);
    LOG_WARNING(Render_Vulkan, "{}x{}x{} ASTC image would be {} MiB decoded; keeping mip levels {}+ ({}x{})",
                info.size.width, info.size.height, info.resources.layers, decoded_bytes(0) >> 20,
                level_offset, host.size.width, host.size.height);
    return host;
}

[[nodiscard]] std::array<VkFormat, 2> ShaderStorageViewFormats(PixelFormat format) {
    if (DefaultBlockWidth(format) != 1 || DefaultBlockHeight(format) != 1) {
        return {};
    }
    switch (BytesPerBlock(format)) {
    case 1:
        return {VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT};
    case 2:
        return {VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT};
    case 4:
        return {VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED};
    case 8:
        return {VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED};
    case 16:
        return {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED};
    default:
        return {};
    }
}

[[nodiscard]] VkImageCreateInfo MakeImageCreateInfo(const Device& device, const ImageInfo& info,
                                                    std::optional<VkFormat> format_override = {}) {
    auto format_info =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format);
    if (format_override) {
        format_info.format = *format_override;
        format_info.attachable = false;
        format_info.storage = true;
    }
    VkImageCreateFlags flags{};
    if (info.type == ImageType::e2D && info.resources.layers >= 6 &&
        info.size.width == info.size.height && !device.HasBrokenCubeImageCompatibility()) {
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (info.type == ImageType::e3D) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    const bool allow_storage =
        info.num_samples == 1 || device.IsStorageImageMultisampleSupported();
    return VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = format_info.format,
        .extent{
            .width = info.size.width >> samples_x,
            .height = info.size.height >> samples_y,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = ConvertSampleCount(info.num_samples),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ImageUsageFlags(format_info, info.format, allow_storage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

[[nodiscard]] VkImageCreateInfo MakeMsaaScratchImageCreateInfo(const Device& device,
                                                               const ImageInfo& info,
                                                               VkImageUsageFlags usage) {
    ImageInfo temp_info = info;
    temp_info.num_samples = 1;
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, temp_info);
    image_ci.format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, info.format).format;
    image_ci.usage = usage;
    return image_ci;
}

[[nodiscard]] vk::Image MakeImage(const Device& device, const MemoryAllocator& allocator,
                                  const ImageInfo& info, std::span<const VkFormat> view_formats,
                                  std::optional<VkFormat> format_override = {},
                                  VkImageUsageFlags host_usage = 0) {
    if (info.type == ImageType::Buffer) {
        return vk::Image{};
    }
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, info, format_override);
    image_ci.usage |= host_usage;
    const VkImageFormatListCreateInfo image_format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (view_formats.size() > 1) {
        image_ci.flags |=
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

        const bool has_storage_compatible_view =
            std::any_of(view_formats.begin(), view_formats.end(), [&device](VkFormat view_format) {
                return device.IsFormatSupported(view_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                                FormatType::Optimal);
            });
        const bool storage_allowed_for_samples =
            image_ci.samples == VK_SAMPLE_COUNT_1_BIT ||
            (device.GetStorageImageSampleCounts() &
             static_cast<VkSampleCountFlags>(image_ci.samples)) != 0;
        if (has_storage_compatible_view && storage_allowed_for_samples) {
            image_ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }

        if (device.IsKhrImageFormatListSupported()) {
            image_ci.pNext = &image_format_list;
        }
    }
    if (unbacked) {
        if (vk::Image image = allocator.CreateUnbackedImage(image_ci, aliased)) {
            return image;
        }
    }
    return allocator.CreateImage(image_ci);
}

[[nodiscard]] VkImageViewType StorageViewType(ImageType type) {
    if (type == ImageType::e3D) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    if (type == ImageType::Linear) {
        return VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
}

[[nodiscard]] vk::ImageView MakeStorageView(const vk::Device& device, u32 level, VkImage image,
                                            VkFormat format,
                                            VkImageViewType view_type) {
    u32 layer_count = VK_REMAINING_ARRAY_LAYERS;
    if (view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
        layer_count = 1;
    }
    static constexpr VkImageViewUsageCreateInfo storage_image_view_usage_create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
    };
    return device.CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &storage_image_view_usage_create_info,
        .flags = 0,
        .image = image,
        .viewType = view_type,
        .format = format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = layer_count,
        },
    });
}

[[nodiscard]] VkImageAspectFlags ImageAspectMask(PixelFormat format) {
    switch (VideoCore::Surface::GetFormatType(format)) {
    case VideoCore::Surface::SurfaceType::ColorTexture:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    case VideoCore::Surface::SurfaceType::Depth:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VideoCore::Surface::SurfaceType::Stencil:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VideoCore::Surface::SurfaceType::DepthStencil:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        ASSERT_MSG(false, "Invalid surface type");
        return VkImageAspectFlags{};
    }
}

[[nodiscard]] bool IsLdrAstcFormat(VkFormat format) {
    return format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}

[[nodiscard]] VkImageAspectFlags ImageViewAspectMask(const VideoCommon::ImageViewInfo& info) {
    if (info.IsRenderTarget()) {
        return ImageAspectMask(info.format);
    }
    bool any_r =
        std::ranges::any_of(info.Swizzle(), [](SwizzleSource s) { return s == SwizzleSource::R; });
    switch (info.format) {
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT_S8_UINT:
        // R = depth, G = stencil
        return any_r ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    case PixelFormat::S8_UINT_D24_UNORM:
        // R = stencil, G = depth
        return any_r ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::D16_UNORM:
    case PixelFormat::D32_FLOAT:
    case PixelFormat::X8_D24_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

[[nodiscard]] VkComponentSwizzle ComponentSwizzle(SwizzleSource swizzle) {
    switch (swizzle) {
    case SwizzleSource::Zero:
        return VK_COMPONENT_SWIZZLE_ZERO;
    case SwizzleSource::R:
        return VK_COMPONENT_SWIZZLE_R;
    case SwizzleSource::G:
        return VK_COMPONENT_SWIZZLE_G;
    case SwizzleSource::B:
        return VK_COMPONENT_SWIZZLE_B;
    case SwizzleSource::A:
        return VK_COMPONENT_SWIZZLE_A;
    case SwizzleSource::OneFloat:
    case SwizzleSource::OneInt:
        return VK_COMPONENT_SWIZZLE_ONE;
    }
    ASSERT_MSG(false, "Invalid swizzle={}", swizzle);
    return VK_COMPONENT_SWIZZLE_ZERO;
}

void SanitizeDepthStencilSwizzle(std::array<SwizzleSource, 4>& swizzle,
                                 bool supports_depth_stencil_swizzle_one) {
    if (supports_depth_stencil_swizzle_one) {
        return;
    }
    std::replace_if(swizzle.begin(), swizzle.end(),
                    [](SwizzleSource value) {
                        return value == SwizzleSource::OneFloat ||
                               value == SwizzleSource::OneInt;
                    },
                    SwizzleSource::Zero);
}

[[nodiscard]] VkImageViewType ImageViewType(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case Shader::TextureType::Color2D:
    case Shader::TextureType::Color2DRect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case Shader::TextureType::ColorCube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case Shader::TextureType::Color3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case Shader::TextureType::ColorArray1D:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case Shader::TextureType::ColorArray2D:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case Shader::TextureType::ColorArrayCube:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case Shader::TextureType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageViewType ImageViewType(VideoCommon::ImageViewType type) {
    switch (type) {
    case VideoCommon::ImageViewType::e1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::Rect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case VideoCommon::ImageViewType::Cube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case VideoCommon::ImageViewType::e3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case VideoCommon::ImageViewType::e1DArray:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case VideoCommon::ImageViewType::e2DArray:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case VideoCommon::ImageViewType::CubeArray:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers(
    VideoCommon::SubresourceLayers subresource, VkImageAspectFlags aspect_mask) {
    return VkImageSubresourceLayers{
        .aspectMask = aspect_mask,
        .mipLevel = static_cast<u32>(subresource.base_level),
        .baseArrayLayer = static_cast<u32>(subresource.base_layer),
        .layerCount = static_cast<u32>(subresource.num_layers),
    };
}

[[nodiscard]] VkOffset3D MakeOffset3D(VideoCommon::Offset3D offset3d) {
    return VkOffset3D{
        .x = offset3d.x,
        .y = offset3d.y,
        .z = offset3d.z,
    };
}

[[nodiscard]] VkExtent3D MakeExtent3D(VideoCommon::Extent3D extent3d) {
    return VkExtent3D{
        .width = static_cast<u32>(extent3d.width),
        .height = static_cast<u32>(extent3d.height),
        .depth = static_cast<u32>(extent3d.depth),
    };
}

[[nodiscard]] VkImageCopy2 MakeImageCopy(const VideoCommon::ImageCopy& copy,
                                         VkImageAspectFlags aspect_mask) noexcept {
    return VkImageCopy2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
        .pNext = nullptr,
        .srcSubresource = MakeImageSubresourceLayers(copy.src_subresource, aspect_mask),
        .srcOffset = MakeOffset3D(copy.src_offset),
        .dstSubresource = MakeImageSubresourceLayers(copy.dst_subresource, aspect_mask),
        .dstOffset = MakeOffset3D(copy.dst_offset),
        .extent = MakeExtent3D(copy.extent),
    };
}

[[nodiscard]] VkBufferImageCopy2 MakeBufferImageCopy(const VideoCommon::ImageCopy& copy,
                                                     bool is_src,
                                                     VkImageAspectFlags aspect_mask) noexcept {
    return VkBufferImageCopy2{
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .pNext = nullptr,
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = MakeImageSubresourceLayers(
            is_src ? copy.src_subresource : copy.dst_subresource, aspect_mask),
        .imageOffset = MakeOffset3D(is_src ? copy.src_offset : copy.dst_offset),
        .imageExtent = MakeExtent3D(copy.extent),
    };
}

[[nodiscard]] boost::container::small_vector<VkBufferImageCopy2, 16> TransformBufferImageCopies(
    std::span<const BufferImageCopy> copies, size_t buffer_offset, VkImageAspectFlags aspect_mask) {
    struct Maker {
        VkBufferImageCopy2 operator()(const BufferImageCopy& copy) const {
            return VkBufferImageCopy2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext = nullptr,
                .bufferOffset = copy.buffer_offset + buffer_offset,
                .bufferRowLength = copy.buffer_row_length,
                .bufferImageHeight = copy.buffer_image_height,
                .imageSubresource =
                    {
                        .aspectMask = aspect_mask,
                        .mipLevel = static_cast<u32>(copy.image_subresource.base_level),
                        .baseArrayLayer = static_cast<u32>(copy.image_subresource.base_layer),
                        .layerCount = static_cast<u32>(copy.image_subresource.num_layers),
                    },
                .imageOffset =
                    {
                        .x = copy.image_offset.x,
                        .y = copy.image_offset.y,
                        .z = copy.image_offset.z,
                    },
                .imageExtent =
                    {
                        .width = copy.image_extent.width,
                        .height = copy.image_extent.height,
                        .depth = copy.image_extent.depth,
                    },
            };
        }
        size_t buffer_offset;
        VkImageAspectFlags aspect_mask;
    };
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        boost::container::small_vector<VkBufferImageCopy2, 16> result(copies.size() * 2);
        std::ranges::transform(copies, result.begin(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_DEPTH_BIT});
        std::ranges::transform(copies, result.begin() + copies.size(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_STENCIL_BIT});
        return result;
    } else {
        boost::container::small_vector<VkBufferImageCopy2, 16> result(copies.size());
        std::ranges::transform(copies, result.begin(), Maker{buffer_offset, aspect_mask});
        return result;
    }
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(VkImageAspectFlags aspect_mask,
                                                           const SubresourceRange& range) {
    return VkImageSubresourceRange{
        .aspectMask = aspect_mask,
        .baseMipLevel = static_cast<u32>(range.base.level),
        .levelCount = static_cast<u32>(range.extent.levels),
        .baseArrayLayer = static_cast<u32>(range.base.layer),
        .layerCount = static_cast<u32>(range.extent.layers),
    };
}

// A guest view's levels on a host image that keeps only levels >= host_level_offset.
[[nodiscard]] SubresourceRange HostRange(const Image& image, SubresourceRange range) {
    const s32 skip = static_cast<s32>(image.host_level_offset);
    if (skip == 0) {
        return range;
    }
    const s32 host_levels = image.info.resources.levels - skip;
    const s32 base = std::clamp(range.base.level - skip, 0, host_levels - 1);
    const s32 end = std::clamp(range.base.level + range.extent.levels - skip, base + 1, host_levels);
    range.base.level = base;
    range.extent.levels = end - base;
    return range;
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(const ImageView* image_view) {
    SubresourceRange range = image_view->range;
    if (True(image_view->flags & VideoCommon::ImageViewFlagBits::Slice)) {
        // Slice image views always affect a single layer, but their subresource range corresponds
        // to the slice. Override the value to affect a single layer.
        range.base.layer = 0;
        range.extent.layers = 1;
    }
    return MakeSubresourceRange(ImageAspectMask(image_view->format), range);
}

[[nodiscard]] VkImageSubresourceLayers MakeSubresourceLayers(const ImageView* image_view) {
    return VkImageSubresourceLayers{
        .aspectMask = ImageAspectMask(image_view->format),
        .mipLevel = static_cast<u32>(image_view->range.base.level),
        .baseArrayLayer = static_cast<u32>(image_view->range.base.layer),
        .layerCount = static_cast<u32>(image_view->range.extent.layers),
    };
}

[[nodiscard]] SwizzleSource ConvertGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapBlueRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::G;
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapSpecial(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::A:
        return SwizzleSource::R;
    case SwizzleSource::R:
        return SwizzleSource::A;
    case SwizzleSource::G:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::G;
    default:
        return value;
    }
}
struct RangedBarrierRange {
    u32 min_mip = (std::numeric_limits<u32>::max)();
    u32 max_mip = (std::numeric_limits<u32>::min)();
    u32 min_layer = (std::numeric_limits<u32>::max)();
    u32 max_layer = (std::numeric_limits<u32>::min)();

    void AddLayers(const VkImageSubresourceLayers& layers) {
        min_mip = (std::min)(min_mip, layers.mipLevel);
        max_mip = (std::max)(max_mip, layers.mipLevel + 1);
        min_layer = (std::min)(min_layer, layers.baseArrayLayer);
        max_layer = (std::max)(max_layer, layers.baseArrayLayer + layers.layerCount);
    }

    VkImageSubresourceRange SubresourceRange(VkImageAspectFlags aspect_mask) const noexcept {
        return VkImageSubresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = min_mip,
            .levelCount = max_mip - min_mip,
            .baseArrayLayer = min_layer,
            .layerCount = max_layer - min_layer,
        };
    }
};
void CopyBufferToImage(vk::CommandBuffer cmdbuf, VkBuffer src_buffer, VkImage image,
                       VkImageAspectFlags aspect_mask, bool is_initialized,
                       std::span<const VkBufferImageCopy2> copies) {
    //  Compute exact mip/layer range being written to
    RangedBarrierRange range;
    for (const auto& region : copies) {
        range.AddLayers(region.imageSubresource);
    }
    const VkImageSubresourceRange subresource_range = range.SubresourceRange(aspect_mask);

    const VkImageMemoryBarrier2 read_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
            .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = is_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = subresource_range,
    };

    const VkImageMemoryBarrier2 write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
            .dstAccessMask = vk::ACCESS_IMAGE_USERS,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = subresource_range,
    };

    cmdbuf.PipelineBarrier(read_barrier);
    cmdbuf.CopyBufferToImage(src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies);
    // TODO: Move this to another API
    cmdbuf.PipelineBarrier(0, {}, {}, write_barrier);
}

[[nodiscard]] VkImageBlit2 MakeImageBlit(const Region2D& dst_region, const Region2D& src_region,
                                         const VkImageSubresourceLayers& dst_layers,
                                         const VkImageSubresourceLayers& src_layers) {
    return VkImageBlit2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .pNext = nullptr,
        .srcSubresource = src_layers,
        .srcOffsets =
            {
                {
                    .x = src_region.start.x,
                    .y = src_region.start.y,
                    .z = 0,
                },
                {
                    .x = src_region.end.x,
                    .y = src_region.end.y,
                    .z = 1,
                },
            },
        .dstSubresource = dst_layers,
        .dstOffsets =
            {
                {
                    .x = dst_region.start.x,
                    .y = dst_region.start.y,
                    .z = 0,
                },
                {
                    .x = dst_region.end.x,
                    .y = dst_region.end.y,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] bool HaveSameExtent(const Region2D& dst_region, const Region2D& src_region) {
    return dst_region.end.x - dst_region.start.x == src_region.end.x - src_region.start.x &&
           dst_region.end.y - dst_region.start.y == src_region.end.y - src_region.start.y;
}

[[nodiscard]] VkImageResolve2 MakeImageResolve(const Region2D& dst_region,
                                               const Region2D& src_region,
                                               const VkImageSubresourceLayers& dst_layers,
                                               const VkImageSubresourceLayers& src_layers) {
    return VkImageResolve2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,
        .pNext = nullptr,
        .srcSubresource = src_layers,
        .srcOffset =
            {
                .x = src_region.start.x,
                .y = src_region.start.y,
                .z = 0,
            },
        .dstSubresource = dst_layers,
        .dstOffset =
            {
                .x = dst_region.start.x,
                .y = dst_region.start.y,
                .z = 0,
            },
        .extent =
            {
                .width = static_cast<u32>(dst_region.end.x - dst_region.start.x),
                .height = static_cast<u32>(dst_region.end.y - dst_region.start.y),
                .depth = 1,
            },
    };
}

void TryTransformSwizzleIfNeeded(PixelFormat format, std::array<SwizzleSource, 4>& swizzle,
                                 bool emulate_bgr565, bool emulate_a4b4g4r4) {
    switch (format) {
    case PixelFormat::A1B5G5R5_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        break;
    case PixelFormat::B5G6R5_UNORM:
        if (emulate_bgr565) {
            std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        }
        break;
    case PixelFormat::A5B5G5R1_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapSpecial);
        break;
    case PixelFormat::G4R4_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapGreenRed);
        break;
    case PixelFormat::A4B4G4R4_UNORM:
        if (emulate_a4b4g4r4) {
            std::ranges::reverse(swizzle);
        }
        break;
    default:
        break;
    }
}



[[nodiscard]] VkFormat Format(Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        break;
    case Shader::ImageFormat::R8_SINT:
        return VK_FORMAT_R8_SINT;
    case Shader::ImageFormat::R8_UINT:
        return VK_FORMAT_R8_UINT;
    case Shader::ImageFormat::R16_UINT:
        return VK_FORMAT_R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return VK_FORMAT_R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return VK_FORMAT_R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return VK_FORMAT_R32G32B32A32_UINT;
    }
    ASSERT_MSG(false, "Invalid image format={}", format);
    return VK_FORMAT_R32_UINT;
}

void BlitScale(Scheduler& scheduler, VkImage src_image, VkImage dst_image, const ImageInfo& info,
               VkImageAspectFlags aspect_mask, const Settings::ResolutionScalingInfo& resolution,
               bool up_scaling = true) {
    const bool is_2d = info.type == ImageType::e2D;
    const auto resources = info.resources;
    const VkExtent2D extent{
        .width = info.size.width,
        .height = info.size.height,
    };
    // Depth and integer formats must use NEAREST filter for blits.
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const VkFilter vk_filter = is_bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, extent, resources, aspect_mask, resolution, is_2d,
                      vk_filter, up_scaling](vk::CommandBuffer cmdbuf) {
        const u32 scaled_width = resolution.ScaleUp(extent.width);
        const u32 scaled_height = is_2d ? resolution.ScaleUp(extent.height) : extent.height;
        const VkOffset2D src_size{
            .x = static_cast<s32>(up_scaling ? extent.width : scaled_width),
            .y = static_cast<s32>(up_scaling ? extent.height : scaled_height),
        };
        const VkOffset2D dst_size{
            .x = static_cast<s32>(up_scaling ? scaled_width : extent.width),
            .y = static_cast<s32>(up_scaling ? scaled_height : extent.height),
        };
        boost::container::small_vector<VkImageBlit2, 4> regions;
        regions.reserve(resources.levels);
        for (s32 level = 0; level < resources.levels; level++) {
            regions.push_back({
                .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                .pNext = nullptr,
                .srcSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .srcOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = (std::max)(1, src_size.x >> level),
                        .y = (std::max)(1, src_size.y >> level),
                        .z = 1,
                    },
                },
                .dstSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .dstOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = (std::max)(1, dst_size.x >> level),
                        .y = (std::max)(1, dst_size.y >> level),
                        .z = 1,
                    },
                },
            });
        }
        const VkImageSubresourceRange subresource_range{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array read_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // Discard contents
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        const std::array write_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        cmdbuf.PipelineBarrier(0, {}, {}, read_barriers);
        cmdbuf.BlitImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions, vk_filter);
        cmdbuf.PipelineBarrier(0, {}, {}, write_barriers);
    });
}

// Uncompressed ASTC needs more video memory than a Switch 2 title leaves on cards under 12 GiB
// (Mario Kart World filled a 16 GB card's 13.5 GB budget): there the textures went unbacked
// (garbage) and the game crashed. Use BC3 for this session; the configuration is left as is.
void UseBc3ForSwitch2OnSmallCards(const Device& device) {
    using namespace Common::Literals;
    if (!Settings::CurrentProgramIsSwitch2() || device.IsOptimalAstcSupported() ||
        Settings::values.astc_recompression.GetValue() != Settings::AstcRecompression::Uncompressed) {
        return;
    }
    const u64 budget = device.GetDeviceMemoryBudget();
    const u64 memory = budget != 0 ? budget : device.GetDeviceLocalMemory();
    const char* keep = std::getenv("EDEN_NX2_KEEP_ASTC");
    if (memory >= 12_GiB || (keep != nullptr && keep[0] == '1')) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "Switch 2 program with {} MiB of video memory: ASTC textures are re-compressed to "
                "BC3 for this session (uncompressed ASTC needs 12 GiB or more). Set "
                "EDEN_NX2_KEEP_ASTC=1 to keep them uncompressed",
                memory >> 20);
    // Per-game value, like the Switch 2 memory layout: RestoreGlobalState undoes it.
    Settings::values.astc_recompression.SetGlobal(false);
    Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Bc3);
}
} // Anonymous namespace

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         MemoryAllocator& memory_allocator_,
                                         StagingBufferPool& staging_buffer_pool_,
                                         BlitImageHelper& blit_image_helper_,
                                         DescriptorPool& descriptor_pool,
                                         ComputePassDescriptorQueue& compute_pass_descriptor_queue)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, blit_image_helper{blit_image_helper_},
      resolution{Settings::values.resolution_info} {
    if (Settings::values.accelerate_astc.GetValue() == Settings::AstcDecodeMode::Gpu) {
        astc_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                  compute_pass_descriptor_queue, memory_allocator);
        if (device.IsTiler()) {
            astc_decoder_fragment_pass.emplace(device, scheduler, descriptor_pool,
                                               compute_pass_descriptor_queue, render_pass_cache);
        }
    }
    if (device.IsKhrImageFormatListSupported() && device.IsStorageBuffer8BitAccessSupported() &&
        device.IsStorageBuffer16BitAccessSupported()) {
        bl_unswizzle_2d_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                     compute_pass_descriptor_queue);
        bl_unswizzle_3d_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                     compute_pass_descriptor_queue);
        pitch_unswizzle_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                     compute_pass_descriptor_queue);
    }
    for (size_t index = 0; index < VideoCore::Surface::MaxPixelFormat; index++) {
        const auto format = static_cast<PixelFormat>(index);
        if (!device.IsHostImageCopySupported() || DefaultBlockWidth(format) == 1) {
            continue;
        }
        const VkFormat vk_format =
            MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, format).format;
        host_copy_formats[index] =
            (device.GetPhysical().GetFormatProperties3(vk_format).optimalTilingFeatures &
             VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT) != 0;
    }
    bl2d_buffer_unswizzle_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                       compute_pass_descriptor_queue);
    if (!device.IsKhrImageFormatListSupported()) {
        return;
    }
    for (size_t index_a = 0; index_a < VideoCore::Surface::MaxPixelFormat; index_a++) {
        const auto image_format = static_cast<PixelFormat>(index_a);
        if (IsPixelFormatASTC(image_format) && !device.IsOptimalAstcSupported()) {
            view_formats[index_a].push_back(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
        }
        for (size_t index_b = 0; index_b < VideoCore::Surface::MaxPixelFormat; index_b++) {
            const auto view_format = static_cast<PixelFormat>(index_b);
            if (VideoCore::Surface::IsViewCompatible(image_format, view_format, false, true)) {
                const auto view_info =
                    MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, view_format);
                view_formats[index_a].push_back(view_info.format);
            }
        }
        auto& formats = view_formats[index_a];
        for (const VkFormat storage_format : ShaderStorageViewFormats(image_format)) {
            if (storage_format == VK_FORMAT_UNDEFINED ||
                std::ranges::find(formats, storage_format) != formats.end()) {
                continue;
            }
            formats.push_back(storage_format);
        }
    }

}

void TextureCacheRuntime::Finish() {
    scheduler.Finish();
}

StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_buffer_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(device, size, MemoryUsage::Download, deferred);
}

void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_buffer_pool.FreeDeferred(ref);
}

bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    if (VideoCore::Surface::GetFormatType(dst.info.format) ==
            VideoCore::Surface::SurfaceType::DepthStencil &&
        !device.IsExtShaderStencilExportSupported()) {
        return true;
    }

    if (VideoCore::Surface::GetFormatType(src.info.format) ==
            VideoCore::Surface::SurfaceType::DepthStencil &&
        !device.IsExtShaderStencilExportSupported()) {
        return true;
    }

    if (dst.info.format == PixelFormat::D32_FLOAT_S8_UINT ||
        src.info.format == PixelFormat::D32_FLOAT_S8_UINT) {
        return true;
    }
    return false;
}

VkBuffer TextureCacheRuntime::GetTemporaryBuffer(size_t needed_size) {
    const size_t wanted_size = (std::max)(needed_size, size_t{1});
    const auto level = (8 * sizeof(size_t)) - std::countl_zero(wanted_size - 1ULL);
    if (buffers[level]) {
        return *buffers[level];
    }
    const auto new_size = std::bit_ceil(needed_size);
    static constexpr VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    const VkBufferCreateInfo temp_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = new_size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    buffers[level] = memory_allocator.CreateBuffer(temp_ci, MemoryUsage::DeviceLocal);
    return *buffers[level];
}

VkImageView TextureCacheRuntime::GetOrCreateResolveShadow(VkImage msaa_image, VkFormat format,
                                                          VkExtent2D extent, u32 layers,
                                                          VkImageAspectFlags aspect_mask) {
    ResolveShadow& shadow = resolve_shadows[msaa_image];
    if (shadow.image && shadow.format == format && shadow.extent.width == extent.width &&
        shadow.extent.height == extent.height && shadow.layers == layers &&
        shadow.aspect_mask == aspect_mask) {
        return *shadow.view;
    }
    VkImageUsageFlags shadow_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        shadow_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        shadow_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (shadow.image) {
        pending_resolve_shadows.emplace_back(scheduler.CurrentTick(), std::move(shadow));
    }
    shadow.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = shadow_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    shadow.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *shadow.image,
        .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    });
    RecordInitialLayout(scheduler, *shadow.image, aspect_mask);
    shadow.format = format;
    shadow.extent = extent;
    shadow.layers = layers;
    shadow.aspect_mask = aspect_mask;
    shadow.up_to_date = false;
    return *shadow.view;
}

VkImageView TextureCacheRuntime::GetOrCreateTirColorScratch(VkImage color_image, VkFormat format,
                                                            VkExtent2D extent, u32 layers,
                                                            VkSampleCountFlagBits samples) {
    TirColorScratch& scratch = tir_color_scratch[color_image];
    if (scratch.image && scratch.format == format && scratch.extent.width == extent.width &&
        scratch.extent.height == extent.height && scratch.layers == layers &&
        scratch.samples == samples) {
        return *scratch.view;
    }
    if (scratch.image) {
        scheduler.Finish();
    }
    scratch.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    scratch.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *scratch.image,
        .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    });
    scratch.format = format;
    scratch.extent = extent;
    scratch.layers = layers;
    scratch.samples = samples;
    return *scratch.view;
}

const TextureCacheRuntime::ResolveShadow* TextureCacheRuntime::GetValidResolveShadow(
    VkImage msaa_image) const {
    const auto it = resolve_shadows.find(msaa_image);
    if (it == resolve_shadows.end() || !it->second.up_to_date || !it->second.image) {
        return nullptr;
    }
    return &it->second;
}

void TextureCacheRuntime::MarkResolveShadowUpToDate(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it != resolve_shadows.end()) {
        it->second.up_to_date = true;
    }
}

void TextureCacheRuntime::InvalidateResolveShadow(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it != resolve_shadows.end()) {
        it->second.up_to_date = false;
    }
}

void TextureCacheRuntime::EraseResolveShadow(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it == resolve_shadows.end()) {
        return;
    }
    if (it->second.image) {
        pending_resolve_shadows.emplace_back(scheduler.CurrentTick(), std::move(it->second));
    }
    resolve_shadows.erase(it);
}

void TextureCacheRuntime::BarrierFeedbackLoop() {
    if (scheduler.HasDepthWrites()) {
        scheduler.RequestOutsideRenderPassOperationContext();
    }
}

void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    if (ENABLE_MSAA_RESOLVE_CONSUME) {
        InvalidateResolveShadow(dst.Handle());
    }
    boost::container::small_vector<VkBufferImageCopy2, 16> vk_in_copies(copies.size());
    boost::container::small_vector<VkBufferImageCopy2, 16> vk_out_copies(copies.size());
    const VkImageAspectFlags src_aspect_mask = src.AspectMask();
    const VkImageAspectFlags dst_aspect_mask = dst.AspectMask();
    // A VkBufferImageCopy region may name only one aspect (VUID-VkBufferImageCopy-aspectMask-09103).
    // Combined depth-stencil images were copied with DEPTH|STENCIL in a single region, which is
    // undefined behaviour and loses the device on NVIDIA (no VK_EXT_shader_stencil_export, so every
    // colour <-> depth-stencil alias lands here). Reinterpret through the depth aspect only.
    const auto copy_aspect = [](VkImageAspectFlags mask) {
        return mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                   ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                   : mask;
    };
    const VkImageAspectFlags src_copy_aspect = copy_aspect(src_aspect_mask);
    const VkImageAspectFlags dst_copy_aspect = copy_aspect(dst_aspect_mask);

    const auto bpp_in = BytesPerBlock(src.info.format) / DefaultBlockWidth(src.info.format);
    const auto bpp_out = BytesPerBlock(dst.info.format) / DefaultBlockWidth(dst.info.format);
    std::ranges::transform(copies, vk_in_copies.begin(),
                           [src_copy_aspect, bpp_in, bpp_out](const auto& copy) {
                               auto copy2 = copy;
                               copy2.src_offset.x = (bpp_out * copy.src_offset.x) / bpp_in;
                               copy2.extent.width = (bpp_out * copy.extent.width) / bpp_in;
                               return MakeBufferImageCopy(copy2, true, src_copy_aspect);
                           });
    std::ranges::transform(copies, vk_out_copies.begin(), [dst_copy_aspect](const auto& copy) {
        return MakeBufferImageCopy(copy, false, dst_copy_aspect);
    });
    const u32 img_bpp = BytesPerBlock(dst.info.format);
    size_t total_size = 0;
    for (const auto& copy : copies) {
        total_size += copy.extent.width * copy.extent.height * copy.extent.depth * img_bpp;
    }
    const VkBuffer copy_buffer = GetTemporaryBuffer(total_size);
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, copy_buffer, src_aspect_mask, dst_aspect_mask,
                      vk_in_copies, vk_out_copies](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkBufferImageCopy2& copy : vk_in_copies) {
            src_range.AddLayers(copy.imageSubresource);
        }
        for (const VkBufferImageCopy2& copy : vk_out_copies) {
            dst_range.AddLayers(copy.imageSubresource);
        }
        static constexpr VkMemoryBarrier2 READ_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = vk::PIPELINE_STAGE_BUFFER_USERS,
            .srcAccessMask = vk::ACCESS_BUFFER_WRITES,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = vk::ACCESS_TRANSFER,
        };
        const std::array pre_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(src_aspect_mask),
            },
        };
        const std::array middle_in_barrier{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(src_aspect_mask),
            },
        };
        const std::array middle_out_barrier{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(dst_aspect_mask),
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(dst_aspect_mask),
            },
        };
        cmdbuf.PipelineBarrier(0, {}, {}, pre_barriers);

        cmdbuf.CopyImageToBuffer(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                                 vk_in_copies);
        cmdbuf.PipelineBarrier(0, {}, {}, middle_in_barrier);

        cmdbuf.PipelineBarrier(0, READ_BARRIER, {}, middle_out_barrier);
        cmdbuf.CopyBufferToImage(copy_buffer, dst_image, VK_IMAGE_LAYOUT_GENERAL, vk_out_copies);
        cmdbuf.PipelineBarrier(0, {}, {}, post_barriers);
    });
    return true;
}

void TextureCacheRuntime::BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    const VkImageAspectFlags aspect_mask = ImageAspectMask(src.format);
    const bool is_dst_msaa = dst.Samples() != VK_SAMPLE_COUNT_1_BIT;
    const bool is_src_msaa = src.Samples() != VK_SAMPLE_COUNT_1_BIT;
    if (aspect_mask != ImageAspectMask(dst.format)) {
        UNIMPLEMENTED_MSG("Incompatible blit from format {} to {}", src.format, dst.format);
        return;
    }
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT && !is_src_msaa && !is_dst_msaa) {
        blit_image_helper.BlitColor(dst_framebuffer, src, dst_region, src_region, filter,
                                    operation);
        return;
    }
    ASSERT(src.format == dst.format);
    const bool is_resolve = is_src_msaa && !is_dst_msaa;
    if (ENABLE_MSAA_RESOLVE_CONSUME && is_resolve && HaveSameExtent(dst_region, src_region)) {
        const VkImageResolve2 resolve = MakeImageResolve(
            dst_region, src_region, MakeSubresourceLayers(&dst), MakeSubresourceLayers(&src));
        const VkImageCopy2 region{
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .pNext = nullptr,
            .srcSubresource = resolve.srcSubresource,
            .srcOffset = resolve.srcOffset,
            .dstSubresource = resolve.dstSubresource,
            .dstOffset = resolve.dstOffset,
            .extent = resolve.extent,
        };
        if (CopyResolveShadow(src.ImageHandle(), dst.ImageHandle(), region)) {
            return;
        }
    }
    if (is_resolve &&
        (aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
            UNIMPLEMENTED_MSG("Stencil-only MSAA resolve is not supported");
            return;
        }
        blit_image_helper.ResolveDepthStencil(dst_framebuffer, src, dst_region, src_region);
        return;
    }
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const auto format = src.format;
        const auto can_blit_depth_stencil = [this, format] {
            switch (format) {
            case VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT:
            case VideoCore::Surface::PixelFormat::S8_UINT_D24_UNORM:
                return device.IsBlitDepth24Stencil8Supported();
            case VideoCore::Surface::PixelFormat::D32_FLOAT_S8_UINT:
                return device.IsBlitDepth32Stencil8Supported();
            default:
                UNREACHABLE();
            }
        }();
        // Use shader-based depth/stencil blits if hardware doesn't support the format
        if (!can_blit_depth_stencil) {
            UNIMPLEMENTED_IF(is_src_msaa || is_dst_msaa);
            blit_image_helper.BlitDepthStencil(dst_framebuffer, src, dst_region, src_region,
                                               filter, operation);
            return;
        }
        UNIMPLEMENTED_IF(is_src_msaa || is_dst_msaa);
        blit_image_helper.BlitDepthStencil(dst_framebuffer, src, dst_region, src_region, filter,
                                           operation);
        return;
    }
    ASSERT(!(is_dst_msaa && !is_src_msaa));
    ASSERT(operation == Fermi2D::Operation::SrcCopy);

    const bool is_msaa_to_msaa = is_src_msaa && is_dst_msaa;
    if (is_msaa_to_msaa && aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
        blit_image_helper.BlitColorMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }
    if (is_msaa_to_msaa) {
        blit_image_helper.BlitDepthStencilMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }

    if (is_resolve && !HaveSameExtent(dst_region, src_region)) {
        blit_image_helper.BlitColorMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }

    const VkImage dst_image = dst.ImageHandle();
    const VkImage src_image = src.ImageHandle();
    const VkImageSubresourceLayers dst_layers = MakeSubresourceLayers(&dst);
    const VkImageSubresourceLayers src_layers = MakeSubresourceLayers(&src);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([filter, dst_region, src_region, dst_image, src_image, dst_layers, src_layers,
                      aspect_mask, is_resolve](vk::CommandBuffer cmdbuf) {
        const std::array read_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        VkImageMemoryBarrier2 write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
            .dstAccessMask = vk::ACCESS_IMAGE_USERS,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(0, {}, {}, read_barriers);
        if (is_resolve) {
            cmdbuf.ResolveImage(src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                MakeImageResolve(dst_region, src_region, dst_layers, src_layers));
        } else {
            const bool is_linear = filter == Fermi2D::Filter::Bilinear;
            const VkFilter vk_filter = is_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            cmdbuf.BlitImage(
                src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                MakeImageBlit(dst_region, src_region, dst_layers, src_layers), vk_filter);
        }
        cmdbuf.PipelineBarrier(write_barrier);
    });
}

void TextureCacheRuntime::ConvertImage(Framebuffer* dst, ImageView& dst_view, ImageView& src_view) {
    switch (dst_view.format) {
    case PixelFormat::R16_UNORM:
        if (src_view.format == PixelFormat::D16_UNORM) {
            return blit_image_helper.ConvertD16ToR16(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_SRGB:
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_UNORM:
        if (src_view.format == PixelFormat::S8_UINT_D24_UNORM) {
            return blit_image_helper.ConvertD24S8ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D24_UNORM_S8_UINT) {
            return blit_image_helper.ConvertS8D24ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::R32_FLOAT:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32ToR32(dst, src_view);
        }
        break;
    case PixelFormat::D16_UNORM:
        if (src_view.format == PixelFormat::R16_UNORM) {
            return blit_image_helper.ConvertR16ToD16(dst, src_view);
        }
        break;
    case PixelFormat::S8_UINT_D24_UNORM:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM) {
            return blit_image_helper.ConvertABGR8ToD24S8(dst, src_view);
        }
        break;
    case PixelFormat::D32_FLOAT:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM ||
            src_view.format == PixelFormat::A8B8G8R8_SRGB ||
            src_view.format == PixelFormat::B8G8R8A8_SRGB) {
            return blit_image_helper.ConvertABGR8ToD32F(dst, src_view);
        }
        if (src_view.format == PixelFormat::R32_FLOAT) {
            return blit_image_helper.ConvertR32ToD32(dst, src_view);
        }
        break;
    case PixelFormat::D24_UNORM_S8_UINT:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM ||
            src_view.format == PixelFormat::A8B8G8R8_SRGB ||
            src_view.format == PixelFormat::B8G8R8A8_SRGB) {
            return blit_image_helper.ConvertABGR8ToD24S8(dst, src_view);
        }
        break;
    default:
        break;
    }

    if (src_view.format == PixelFormat::D32_FLOAT &&
        VideoCore::Surface::GetFormatType(dst_view.format) == SurfaceType::ColorTexture &&
        (dst_view.format == PixelFormat::B5G6R5_UNORM ||
         Settings::values.fix_bloom_effects.GetValue())) {
        const Region2D region{
            .start = {0, 0},
            .end = {static_cast<s32>(dst->RenderArea().width),
                    static_cast<s32>(dst->RenderArea().height)},
        };
        return blit_image_helper.BlitColor(dst, src_view, region, region,
                                        Tegra::Engines::Fermi2D::Filter::Point,
                                        Tegra::Engines::Fermi2D::Operation::SrcCopy);
    }

    LOG_DEBUG(Render_Vulkan, "Unimplemented texture conversion from {} to {} format type", src_view.format, dst_view.format);
}

VkFormat TextureCacheRuntime::GetSupportedFormat(VkFormat requested_format,
                                                VkFormatFeatureFlags required_features) const {
    if (requested_format == VK_FORMAT_A8B8G8R8_SRGB_PACK32 &&
        (required_features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        // Force valid depth format when sRGB requested in depth context
        return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return requested_format;
}

// Helper functions for format compatibility checks
bool TextureCacheRuntime::IsFormatDitherable(PixelFormat format) {
    switch (format) {
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::A8B8G8R8_SRGB:
        return true;
    default:
        return false;
    }
}

bool TextureCacheRuntime::IsFormatScalable(PixelFormat format) {
    switch (format) {
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::R16G16B16A16_FLOAT:
    case PixelFormat::R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    if (ENABLE_MSAA_RESOLVE_CONSUME) {
        InvalidateResolveShadow(dst.Handle());
    }
    // As per the size-compatible formats section of vulkan, copy manually via ReinterpretImage
    // these images that aren't size-compatible
    if (BytesPerBlock(src.info.format) != BytesPerBlock(dst.info.format)) {
#ifdef _WIN32
        // On Windows, linear images cause device loss when used in image copies.
        // Tested with TitleID: 0x010067300059A00 (Mario + Rabbids Kingdom Battle)
        if (src.info.type == ImageType::Linear || dst.info.type == ImageType::Linear) {
            return;
        }
#endif
        auto oneCopy = VideoCommon::ImageCopy{
            .src_offset = VideoCommon::Offset3D(0, 0, 0),
            .dst_offset = VideoCommon::Offset3D(0, 0, 0),
            .extent = dst.info.size
        };
        return ReinterpretImage(dst, src, std::span{&oneCopy, 1});
    }
    boost::container::small_vector<VkImageCopy2, 16> vk_copies(copies.size());
    const VkImageAspectFlags aspect_mask = dst.AspectMask();
    ASSERT(aspect_mask == src.AspectMask());

    std::ranges::transform(copies, vk_copies.begin(), [aspect_mask](const auto& copy) {
        return MakeImageCopy(copy, aspect_mask);
    });
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, aspect_mask, vk_copies](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkImageCopy2& copy : vk_copies) {
            dst_range.AddLayers(copy.dstSubresource);
            src_range.AddLayers(copy.srcSubresource);
        }
        const std::array pre_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(aspect_mask),
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(aspect_mask),
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(aspect_mask),
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(aspect_mask),
            },
        };
        cmdbuf.PipelineBarrier(0, {}, {}, pre_barriers);
        cmdbuf.CopyImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VideoCommon::FixSmallVectorADL(vk_copies));
        cmdbuf.PipelineBarrier(0, {}, {}, post_barriers);
    });
}

bool TextureCacheRuntime::CopyResolveShadow(VkImage msaa_image, VkImage dst_image,
                                            const VkImageCopy2& region) {
    const ResolveShadow* const shadow = GetValidResolveShadow(msaa_image);
    if (shadow == nullptr || shadow->aspect_mask != region.srcSubresource.aspectMask ||
        region.srcSubresource.mipLevel != 0 || region.srcOffset.x < 0 || region.srcOffset.y < 0 ||
        static_cast<u32>(region.srcOffset.x) + region.extent.width > shadow->extent.width ||
        static_cast<u32>(region.srcOffset.y) + region.extent.height > shadow->extent.height ||
        region.srcSubresource.baseArrayLayer + region.srcSubresource.layerCount >
            shadow->layers) {
        return false;
    }
    const VkImageAspectFlags aspect_mask = shadow->aspect_mask;
    const VkImage shadow_image = *shadow->image;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([shadow_image, dst_image, region, aspect_mask](vk::CommandBuffer cmdbuf) {
        const std::array pre_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = shadow_image,
                .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS},
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS},
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = shadow_image,
                .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS},
            },
            VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS},
            },
        };
        cmdbuf.PipelineBarrier(0, {}, {}, pre_barriers);
        cmdbuf.CopyImage(shadow_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        cmdbuf.PipelineBarrier(0, {}, {}, post_barriers);
    });
    const bool is_color = aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT;
    if ((is_color && ENABLE_MSAA_COLOR_DISCARD) ||
        (!is_color && ENABLE_MSAA_DEPTH_STENCIL_DISCARD)) {
        scheduler.DiscardResolvedAttachments(*shadow->view);
    }
    return true;
}

void TextureCacheRuntime::CopyImageMSAA(Image& dst, Image& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    const bool msaa_to_non_msaa = src.info.num_samples > 1 && dst.info.num_samples == 1;
    const u32 num_samples = msaa_to_non_msaa ? src.info.num_samples : dst.info.num_samples;
    if (ENABLE_MSAA_RESOLVE_CONSUME && msaa_to_non_msaa && copies.size() == 1 &&
        src.info.format == dst.info.format && copies.front().src_offset.x == 0 &&
        copies.front().src_offset.y == 0) {
        const VideoCommon::ImageCopy& copy = copies.front();
        const VkImageCopy2 region{
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .pNext = nullptr,
            .srcSubresource{
                .aspectMask = dst.AspectMask(),
                .mipLevel = static_cast<u32>(copy.src_subresource.base_level),
                .baseArrayLayer = static_cast<u32>(copy.src_subresource.base_layer),
                .layerCount = static_cast<u32>(copy.src_subresource.num_layers),
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = dst.AspectMask(),
                .mipLevel = static_cast<u32>(copy.dst_subresource.base_level),
                .baseArrayLayer = static_cast<u32>(copy.dst_subresource.base_layer),
                .layerCount = static_cast<u32>(copy.dst_subresource.num_layers),
            },
            .dstOffset = {copy.dst_offset.x, copy.dst_offset.y, copy.dst_offset.z},
            .extent = {copy.extent.width, copy.extent.height, 1},
        };
        if (CopyResolveShadow(src.Handle(), dst.Handle(), region)) {
            return;
        }
    }
    const VkImageAspectFlags dst_aspect_mask = dst.AspectMask();
    if ((dst_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
        const bool copies_stencil = (dst_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                                    device.IsExtShaderStencilExportSupported();
        blit_image_helper.CopyMSAADepth(dst.Handle(), dst.info.format, src.Handle(),
                                        src.info.format, num_samples, copies, copies_stencil,
                                        msaa_to_non_msaa);
        return;
    }
    if ((dst_aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
        UNIMPLEMENTED_MSG("Copying images with different samples is not supported.");
        return;
    }
    blit_image_helper.CopyMSAA(dst.Handle(), dst.info.format, src.Handle(), src.info.format,
                               num_samples, copies, msaa_to_non_msaa);
}

u64 TextureCacheRuntime::CurrentSyncPoint() const noexcept {
    return scheduler.CurrentTick();
}

u64 TextureCacheRuntime::CompletedSyncPoint() const {
    auto& master_semaphore = scheduler.GetMasterSemaphore();
    master_semaphore.Refresh();
    return master_semaphore.KnownGpuTick();
}

void TextureCacheRuntime::WaitSyncPoint(u64 sync_point) {
    scheduler.Wait(sync_point);
}

u64 TextureCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool TextureCacheRuntime::CanDownloadMsaa(const VideoCommon::ImageInfo& info) const {
    const VkImageAspectFlags aspect_mask = ImageAspectMask(info.format);
    if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        return true;
    }
    if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
        return false;
    }
    if ((aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
        return device.IsExtShaderStencilExportSupported();
    }
    return true;
}

bool TextureCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

void TextureCacheRuntime::LogMemoryStatistics() const {
    Vulkan::LogMemoryStatistics(device);
}

bool TextureCacheRuntime::UseOpaqueAstcFormat() const {
    // EDEN_NX2_ASTC_BC1=0 keeps every re-compressed ASTC image in BC3.
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ASTC_BC1");
        return value == nullptr || value[0] != '0';
    }();
    return enabled && !device.IsOptimalAstcSupported() &&
           Settings::values.astc_recompression.GetValue() == Settings::AstcRecompression::Bc3;
}

void TextureCacheRuntime::FlushDeferredClear() {
    scheduler.FlushDeferredClear();
}

VkImage TextureCacheRuntime::AcquireMsaaScratchImage(const VkImageCreateInfo& image_ci) {
    const MsaaScratchKey key{
        .format = image_ci.format,
        .type = image_ci.imageType,
        .width = image_ci.extent.width,
        .height = image_ci.extent.height,
        .depth = image_ci.extent.depth,
        .levels = image_ci.mipLevels,
        .layers = image_ci.arrayLayers,
        .usage = image_ci.usage,
        .flags = image_ci.flags,
    };
    for (MsaaScratchImage& scratch : msaa_scratch_images) {
        if (scratch.key != key || !scheduler.IsFree(scratch.tick)) {
            continue;
        }
        scratch.tick = (std::numeric_limits<u64>::max)();
        scratch.unused_frames = 0;
        return *scratch.image;
    }
    MsaaScratchImage scratch{
        .key = key,
        .image = memory_allocator.CreateImage(image_ci),
        .tick = (std::numeric_limits<u64>::max)(),
        .unused_frames = 0,
    };
    const VkImage handle = *scratch.image;
    msaa_scratch_images.push_back(std::move(scratch));
    return handle;
}

void TextureCacheRuntime::ReleaseMsaaScratchImage(VkImage image) {
    for (MsaaScratchImage& scratch : msaa_scratch_images) {
        if (*scratch.image == image) {
            scratch.tick = scheduler.CurrentTick();
            return;
        }
    }
}

void TextureCacheRuntime::TickFrame() {
    static constexpr u32 MAX_UNUSED_SCRATCH_FRAMES = 60;
    std::erase_if(msaa_scratch_images, [this](MsaaScratchImage& scratch) {
        if (!scheduler.IsFree(scratch.tick)) {
            scratch.unused_frames = 0;
            return false;
        }
        return ++scratch.unused_frames > MAX_UNUSED_SCRATCH_FRAMES;
    });
    std::erase_if(pending_resolve_shadows, [this](const auto& pending) {
        return scheduler.IsFree(pending.first);
    });
}

Image::Image(TextureCacheRuntime& runtime_, const ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase(info_, gpu_addr_, cpu_addr_), scheduler{&runtime_.scheduler},
      runtime{&runtime_}, aspect_mask(ImageAspectMask(info.format)) {
    if (IsPixelFormatASTC(info.format) && !runtime->device.IsOptimalAstcSupported()) {
        switch (Settings::values.accelerate_astc.GetValue()) {
        case Settings::AstcDecodeMode::Gpu:
            if (WillUseAcceleratedAstcDecode(runtime->device, info)) {
                flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
                break;
            }
            [[fallthrough]];
        case Settings::AstcDecodeMode::CpuAsynchronous:
            flags |= VideoCommon::ImageFlagBits::AsynchronousDecode;
            break;
        default:
            break;
        }
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    } else if (runtime->bl3db_unswizzle_pass &&
               BlockLinearUnswizzle3DBufferPass::IsSupported(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    } else if (runtime->bl2d_unswizzle_pass && BlockLinearUnswizzle2DPass::IsSupported(info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    } else if (runtime->bl3db_unswizzle_pass &&
               BlockLinearUnswizzle3DBufferPass::IsSupported(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    } else if (runtime->bl2d_unswizzle_pass &&
               BlockLinearUnswizzle2DPass::IsSupported(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    } else if (runtime->bl3db_unswizzle_pass &&
               BlockLinearUnswizzle3DBufferPass::IsSupported(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
    if (IsPixelFormatBCn(info.format) && !runtime->device.IsOptimalBcnSupported()) {
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    if (False(flags & VideoCommon::ImageFlagBits::Converted) &&
        WillUseAcceleratedUnswizzle(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
    VkImageUsageFlags host_usage{};
    if (False(flags & VideoCommon::ImageFlagBits::Converted) &&
        runtime->host_copy_formats[static_cast<size_t>(info.format)]) {
        host_usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
    }
    original_image = MakeImage(runtime->device, runtime->memory_allocator, info,
                               runtime->ViewFormats(info.format), {}, host_usage);
    if (runtime->device.HasDebuggingToolAttached()) {
        original_image.SetObjectNameEXT(VideoCommon::Name(*this).c_str());
    }
    if (False(flags & VideoCommon::ImageFlagBits::Converted) &&
        SupportsAcceleratedUnswizzle(runtime->device, info)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
    current_image = &Image::original_image;
    storage_image_views.resize(info.resources.levels);
    if (original_image) {
        runtime->TransitionImageLayout(*this);
    }
}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::~Image() {
    if (ENABLE_MSAA_RESOLVE_CONSUME && runtime != nullptr) {
        if (original_image) {
            runtime->EraseResolveShadow(*original_image);
        }
        if (scaled_image) {
            runtime->EraseResolveShadow(*scaled_image);
        }
    }
}

void Image::UploadMemory(VkBuffer buffer, VkDeviceSize offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    // TODO: Move this to another API
    if (ENABLE_MSAA_RESOLVE_CONSUME && runtime != nullptr) {
        runtime->InvalidateResolveShadow(Handle());
    }
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ScaleDown(true);
    }

    const bool msaa_upload_is_depth = (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const bool wants_msaa_upload = info.num_samples > 1
        && ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != 0 || msaa_upload_is_depth);

    if (wants_msaa_upload) {
        const bool msaa_upload_copies_stencil =
            msaa_upload_is_depth && (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
            runtime->device.IsExtShaderStencilExportSupported();
        VkImageAspectFlags upload_aspect_mask = aspect_mask;
        if (msaa_upload_is_depth) {
            upload_aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (msaa_upload_copies_stencil) {
                upload_aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        const VkImageCreateInfo image_ci = MakeMsaaScratchImageCreateInfo(
            runtime->device, info, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const VkImage temp_vk_image = runtime->AcquireMsaaScratchImage(image_ci);

        scheduler->RequestOutsideRenderPassOperationContext();
        auto vk_copies = TransformBufferImageCopies(copies, offset, upload_aspect_mask);
        const VkBuffer src_buffer = buffer;
        const VkImageAspectFlags vk_aspect_mask = upload_aspect_mask;

        scheduler->Record([src_buffer, temp_vk_image, vk_aspect_mask,
                           vk_copies](vk::CommandBuffer cmdbuf) {
            CopyBufferToImage(cmdbuf, src_buffer, temp_vk_image, vk_aspect_mask, false, vk_copies);
        });

        const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
        boost::container::small_vector<VideoCommon::ImageCopy, 16> image_copies;
        image_copies.reserve(copies.size());
        for (const auto& copy : copies) {
            VideoCommon::ImageCopy image_copy{};
            image_copy.src_offset = {0, 0, 0};
            image_copy.dst_offset = {copy.image_offset.x >> samples_x,
                                     copy.image_offset.y >> samples_y, copy.image_offset.z};
            image_copy.src_subresource = copy.image_subresource;
            image_copy.dst_subresource = copy.image_subresource;
            image_copy.extent = {copy.image_extent.width >> samples_x,
                                 copy.image_extent.height >> samples_y, copy.image_extent.depth};
            image_copies.push_back(image_copy);
        }

        if (msaa_upload_is_depth) {
            runtime->blit_image_helper.CopyMSAADepth(Handle(), info.format, temp_vk_image,
                                                     info.format, info.num_samples,
                                                     {image_copies.data(), image_copies.size()},
                                                     msaa_upload_copies_stencil, false);
        } else {
            runtime->blit_image_helper.CopyMSAA(Handle(), info.format, temp_vk_image, info.format,
                                                info.num_samples,
                                                {image_copies.data(), image_copies.size()}, false);
        }
        InitializationFor(current_image) = true;
        runtime->ReleaseMsaaScratchImage(temp_vk_image);

        if (is_rescaled) {
            ScaleUp();
        }
        return;
    }

    if (info.num_samples > 1) {
        LOG_WARNING(Render_Vulkan, "MSAA upload not implemented for format {}", info.format);
        if (runtime != nullptr) {
            runtime->TransitionImageLayout(*this);
        }
        if (is_rescaled) {
            ScaleUp();
        }
        return;
    }

    auto vk_copies = TransformBufferImageCopies(copies, offset, aspect_mask);
    const VkBuffer src_buffer = buffer;
    const VkImage vk_image = *original_image;
    const VkImageAspectFlags vk_aspect_mask = aspect_mask;
    const bool was_initialized = std::exchange(InitializationFor(&Image::original_image), true);

    auto copy = [src_buffer, vk_image, vk_aspect_mask, was_initialized,
                 vk_copies](vk::CommandBuffer cmdbuf) {
        CopyBufferToImage(cmdbuf, src_buffer, vk_image, vk_aspect_mask, was_initialized, VideoCommon::FixSmallVectorADL(vk_copies));
    };
    if (True(flags & ImageFlagBits::ReorderableUpload)) {
        scheduler->RecordWithUploadBuffer(
            [copy = std::move(copy)](vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
                copy(upload_cmdbuf);
            });
    } else {
        scheduler->RequestOutsideRenderPassOperationContext();
        scheduler->Record(std::move(copy));
    }

    if (is_rescaled) {
        ScaleUp();
    }
}

bool Image::UploadSwizzledFromBuffer(VkBuffer buffer, VkDeviceSize offset) {
    if (runtime == nullptr || !runtime->bl2d_buffer_unswizzle_pass || offset % 4 != 0 ||
        guest_size_bytes > runtime->device.GetMaxStorageBufferRange() ||
        !BlockLinearUnswizzle2DBufferPass::IsSupported(*this)) {
        return false;
    }
    if (ENABLE_MSAA_RESOLVE_CONSUME) {
        runtime->InvalidateResolveShadow(Handle());
    }
    auto [linear_buffer, copies] =
        runtime->bl2d_buffer_unswizzle_pass->Unswizzle(*this, buffer, offset);
    const VkImage vk_image = *original_image;
    const VkImageAspectFlags vk_aspect_mask = aspect_mask;
    const bool was_initialized = std::exchange(initialized, true);
    scheduler->Record([linear_buffer, vk_image, vk_aspect_mask, was_initialized,
                       copies = std::move(copies)](vk::CommandBuffer cmdbuf) {
        CopyBufferToImage(cmdbuf, linear_buffer, vk_image, vk_aspect_mask, was_initialized, copies);
    });
    return true;
}

void Image::UploadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    UploadMemory(map.buffer, map.offset, copies);
}

void Image::UploadHostMemory(std::span<const u8> memory,
                             std::span<const BufferImageCopy> copies) {
    const auto vk_copies = TransformBufferImageCopies(copies, 0, aspect_mask);
    boost::container::small_vector<VkMemoryToImageCopyEXT, 16> regions;
    for (const VkBufferImageCopy2& copy : vk_copies) {
        regions.push_back(VkMemoryToImageCopyEXT{
            .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT,
            .pNext = nullptr,
            .pHostPointer = memory.data() + copy.bufferOffset,
            .memoryRowLength = copy.bufferRowLength,
            .memoryImageHeight = copy.bufferImageHeight,
            .imageSubresource = copy.imageSubresource,
            .imageOffset = copy.imageOffset,
            .imageExtent = copy.imageExtent,
        });
    }
    runtime->device.GetLogical().CopyMemoryToImageEXT(VkCopyMemoryToImageInfoEXT{
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .dstImage = *original_image,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = static_cast<u32>(regions.size()),
        .pRegions = regions.data(),
    });
}

void Image::DownloadMemory(VkBuffer buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    std::array buffer_handles{
        buffer,
    };
    std::array buffer_offsets{
        offset,
    };
    DownloadMemory(buffer_handles, buffer_offsets, copies);
}

void Image::DownloadMemory(std::span<VkBuffer> buffers_span, std::span<size_t> offsets_span,
                            std::span<const VideoCommon::BufferImageCopy> copies) {
    if (!original_image) {
        LOG_CRITICAL(Render_Vulkan, "Missing image at download: gpu={:#x}, cpu={:#x}, type={}, format={}, flags={:#x}, size={}, dimensions={}x{}",
                     gpu_addr, cpu_addr, static_cast<u32>(info.type), static_cast<u32>(info.format),
                     static_cast<u32>(flags), unswizzled_size_bytes, info.size.width, info.size.height);
    }
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ScaleDown();
    }

    if (info.num_samples > 1) {
        if (runtime->CanDownloadMsaa(info)) {
            const bool msaa_download_is_depth =
                (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
            const bool msaa_download_copies_stencil =
                msaa_download_is_depth && (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                runtime->device.IsExtShaderStencilExportSupported();
            VkImageUsageFlags scratch_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (msaa_download_is_depth) {
                scratch_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            } else {
                scratch_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            const VkImageCreateInfo image_ci =
                MakeMsaaScratchImageCreateInfo(runtime->device, info, scratch_usage);
            const VkImage temp_vk_image = runtime->AcquireMsaaScratchImage(image_ci);

            const VkImageAspectFlags temp_aspect_mask = aspect_mask;
            const VkAccessFlags2 attachment_access =
                msaa_download_is_depth ? (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                       : (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const VkPipelineStageFlags2 attachment_stage =
                msaa_download_is_depth ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                                       : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

            scheduler->RequestOutsideRenderPassOperationContext();
            scheduler->Record([temp_vk_image, temp_aspect_mask, attachment_access,
                               attachment_stage](vk::CommandBuffer cmdbuf) {
                const VkImageMemoryBarrier2 init_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    .srcAccessMask = 0,
                    .dstStageMask = attachment_stage,
                    .dstAccessMask = attachment_access,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = temp_vk_image,
                    .subresourceRange{
                        .aspectMask = temp_aspect_mask,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(init_barrier);
            });

            boost::container::small_vector<VideoCommon::ImageCopy, 16> image_copies;
            image_copies.reserve(copies.size());
            for (const auto& copy : copies) {
                VideoCommon::ImageCopy image_copy;
                image_copy.src_offset = copy.image_offset;
                image_copy.dst_offset = copy.image_offset;
                image_copy.src_subresource = copy.image_subresource;
                image_copy.dst_subresource = copy.image_subresource;
                image_copy.extent = copy.image_extent;
                image_copies.push_back(image_copy);
            }

            if (msaa_download_is_depth) {
                runtime->blit_image_helper.CopyMSAADepth(
                    temp_vk_image, info.format, Handle(), info.format, info.num_samples,
                    {image_copies.data(), image_copies.size()}, msaa_download_copies_stencil, true);
            } else {
                runtime->blit_image_helper.CopyMSAA(temp_vk_image, info.format, Handle(),
                                                    info.format, info.num_samples,
                                                    {image_copies.data(), image_copies.size()},
                                                    true);
            }

            boost::container::small_vector<VkBuffer, 8> buffers_vector{};
            boost::container::small_vector<
                boost::container::small_vector<VkBufferImageCopy2, 16>, 8>
                vk_copies;
            for (size_t index = 0; index < buffers_span.size(); index++) {
                buffers_vector.emplace_back(buffers_span[index]);
                vk_copies.emplace_back(
                    TransformBufferImageCopies(copies, offsets_span[index], aspect_mask));
            }

            scheduler->RequestOutsideRenderPassOperationContext();
            scheduler->Record([buffers = std::move(buffers_vector), image = temp_vk_image,
                               aspect_mask_ = aspect_mask, vk_copies,
                               consumer_stages = runtime->device.GetBufferConsumerStages(),
                               consumer_access = runtime->device.GetBufferConsumerAccess()](
                                  vk::CommandBuffer cmdbuf) {
                const VkImageMemoryBarrier2 read_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                    .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                    .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange{
                        .aspectMask = aspect_mask_,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(read_barrier);

                for (size_t index = 0; index < buffers.size(); index++) {
                    cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[index],
                                             vk_copies[index]);
                }

                const VkMemoryBarrier2 memory_write_barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = consumer_stages,
                    .dstAccessMask = consumer_access,
                };
                const VkImageMemoryBarrier2 image_write_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = nullptr,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = 0,
                    .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                    .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange{
                        .aspectMask = aspect_mask_,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(0, memory_write_barrier, {}, image_write_barrier);
            });
            runtime->ReleaseMsaaScratchImage(temp_vk_image);
        }
        if (is_rescaled) {
            ScaleUp(true);
        }
        return;
    } else {
        boost::container::small_vector<VkBuffer, 8> buffers_vector{};
        boost::container::small_vector<boost::container::small_vector<VkBufferImageCopy2, 16>, 8>
            vk_copies;
        for (size_t index = 0; index < buffers_span.size(); index++) {
            buffers_vector.emplace_back(buffers_span[index]);
            vk_copies.emplace_back(
                TransformBufferImageCopies(copies, offsets_span[index], aspect_mask));
        }
        scheduler->RequestOutsideRenderPassOperationContext();
        scheduler->Record([buffers = std::move(buffers_vector), image = *original_image,
                           aspect_mask_ = aspect_mask, vk_copies,
                           consumer_stages = runtime->device.GetBufferConsumerStages(),
                           consumer_access = runtime->device.GetBufferConsumerAccess()](
                              vk::CommandBuffer cmdbuf) {
            const VkImageMemoryBarrier2 read_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .srcAccessMask = vk::ACCESS_IMAGE_WRITES,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = aspect_mask_,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            cmdbuf.PipelineBarrier(read_barrier);

            for (size_t index = 0; index < buffers.size(); index++) {
                cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[index],
                                         vk_copies[index]);
            }

            const VkMemoryBarrier2 memory_write_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = consumer_stages,
                .dstAccessMask = consumer_access,
            };
            const VkImageMemoryBarrier2 image_write_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask = vk::PIPELINE_STAGE_IMAGE_USERS,
                .dstAccessMask = vk::ACCESS_IMAGE_USERS,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = aspect_mask_,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            cmdbuf.PipelineBarrier(0, memory_write_barrier, {}, image_write_barrier);
        });
    }

    if (is_rescaled) {
        ScaleUp(true);
    }
}

void Image::DownloadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    std::array buffers{
        map.buffer,
    };
    std::array offsets{
        static_cast<size_t>(map.offset),
    };
    DownloadMemory(buffers, offsets, copies);
}

std::vector<vk::ImageView>& Image::StorageViewsFor(vk::Image Image::*image) {
    if (image == &Image::scaled_image) {
        if (scaled_storage_image_views.empty()) {
            scaled_storage_image_views.resize(info.resources.levels);
        }
        return scaled_storage_image_views;
    }
    return storage_image_views;
}

bool& Image::InitializationFor(vk::Image Image::*image) noexcept {
    if (image == &Image::scaled_image) {
        return scaled_initialized;
    }
    SparseLayerMemory& s = sparse_layers;
    s.allocator = &runtime->memory_allocator;
    s.requirements = device.GetLogical().GetImageMemoryRequirements(handle);
    s.sparse = *it;
    u32 offset = host_level_offset;
    const ImageInfo host = TrimmedHostInfo(device, info, offset);
    s.width = host.size.width;
    s.height = host.size.height;
    s.levels = static_cast<u32>(host.resources.levels);
    // Sparse block size in texels (drivers give it in texels or in compressed blocks).
    const bool huge_astc = IsHugeAstcArray(device, info);
    const u32 block_width = huge_astc ? 1 : VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 block_height = huge_astc ? 1 : VideoCore::Surface::DefaultBlockHeight(info.format);
    const u64 block_bytes = huge_astc ? 4 : VideoCore::Surface::BytesPerBlock(info.format);
    VkExtent3D granularity = it->formatProperties.imageGranularity;
    if (granularity.width == 0 || granularity.height == 0) {
        // Seen with AMD's driver: the sparse block size comes back as 0, and the size computation
        // below divided by it. Use the standard 2D block shape when the device promises it.
        if (!StandardSparseBlockShape(device, block_bytes, granularity)) {
            LOG_WARNING(Render_Vulkan,
                        "Sparse layered image {:#x}: the driver reports no sparse block size and "
                        "no standard block shape; not using sparse layers",
                        gpu_addr);
            return false;
        }
        granularity.width *= block_width;
        granularity.height *= block_height;
        LOG_INFO(Render_Vulkan,
                 "Sparse layered image {:#x}: no block size from the driver, using the standard "
                 "{}x{} texel block",
                 gpu_addr, granularity.width, granularity.height);
    }
    s.tile_width = granularity.width;
    s.tile_height = granularity.height;
    const u64 tile = s.requirements.alignment;
    if (u64{granularity.width / block_width} * (granularity.height / block_height) * block_bytes !=
            tile &&
        u64{granularity.width} * granularity.height * block_bytes == tile) {
        s.tile_width *= block_width;
        s.tile_height *= block_height;
    }
    const u32 tail_lod = (std::min)(it->imageMipTailFirstLod, s.levels);
    VkDeviceSize bytes = 0;
    for (u32 level = 0; level < tail_lod; ++level) {
        const u32 width = (std::max)(s.width >> level, 1U);
        const u32 height = (std::max)(s.height >> level, 1U);
        bytes += u64{Common::DivCeil(width, s.tile_width)} * Common::DivCeil(height, s.tile_height) *
                 tile;
    }
    if (tail_lod < s.levels &&
        (it->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) == 0) {
        bytes += it->imageMipTailSize;
    }
    s.layer_bytes = Common::AlignUp(bytes, tile);
    s.layer_memory.assign(static_cast<size_t>(info.resources.layers), VK_NULL_HANDLE);
    LOG_INFO(Render_Vulkan,
             "Sparse layered image {:#x}: {}x{} {} levels {} layers, {} KiB a layer, block {}x{}",
             gpu_addr, s.width, s.height, s.levels, info.resources.layers, s.layer_bytes >> 10,
             s.tile_width, s.tile_height);
    return true;
}

VkImageView Image::StorageImageView(s32 level) noexcept {
    const bool astc_decode = WillUseAcceleratedAstcDecode(runtime->device, info);
    const bool unswizzle_upload =
        !astc_decode && True(flags & ImageFlagBits::AcceleratedUpload);
    vk::Image Image::*target = current_image;
    if (astc_decode || unswizzle_upload) {
        target = &Image::original_image;
    }
    auto& view = StorageViewsFor(target)[level];
    if (!view) {
        auto format_info =
            MaxwellToVK::SurfaceFormat(runtime->device, FormatType::Optimal, true, info.format);
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (WillUseAcceleratedAstcDecode(runtime->device, info)) {
            format_info.format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        } else if (True(flags & VideoCommon::ImageFlagBits::AcceleratedUpload)) {
            format_info.format = UnswizzleStorageFormat(BytesPerBlock(info.format));
            if (info.type == ImageType::e3D) {
                view_type = VK_IMAGE_VIEW_TYPE_3D;
            } else if (info.type == ImageType::Linear) {
                view_type = VK_IMAGE_VIEW_TYPE_2D;
            }
        }
        view = MakeStorageView(runtime->device.GetLogical(), level, *original_image,
                               format_info.format, view_type);
    }
    return *view;
}

bool Image::IsRescaled() const noexcept {
    return True(flags & ImageFlagBits::Rescaled);
}

bool Image::ScaleUp(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (True(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags |= ImageFlagBits::Rescaled;
    has_scaled = true;
    if (!scaled_image) {
        const bool is_2d = info.type == ImageType::e2D;
        const u32 scaled_width = resolution.ScaleUp(info.size.width);
        const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
        auto scaled_info = info;
        scaled_info.size.width = scaled_width;
        scaled_info.size.height = scaled_height;
        scaled_image = MakeImage(runtime->device, runtime->memory_allocator, scaled_info,
                                 runtime->ViewFormats(info.format));
        ignore = false;
    }
    current_image = &Image::scaled_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        if (!BlitScaleHelper(true)) {
            flags &= ~ImageFlagBits::Rescaled;
            current_image = &Image::original_image;
            return false;
        }
    } else {
        BlitScale(*scheduler, *original_image, *scaled_image, info, aspect_mask, resolution);
    }
    return true;
}

bool Image::ScaleDown(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (False(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags &= ~ImageFlagBits::Rescaled;
    current_image = &Image::original_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        if (!BlitScaleHelper(false)) {
            current_image = &Image::scaled_image;
            flags |= ImageFlagBits::Rescaled;
            return false;
        }
    } else {
        BlitScale(*scheduler, *scaled_image, *original_image, info, aspect_mask, resolution, false);
    }
    return true;
}

bool Image::BlitScaleHelper(bool scale_up) {
    using namespace VideoCommon;
    static constexpr auto BLIT_OPERATION = Tegra::Engines::Fermi2D::Operation::SrcCopy;
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const auto operation = is_bilinear ? Tegra::Engines::Fermi2D::Filter::Bilinear
                                       : Tegra::Engines::Fermi2D::Filter::Point;

    const bool is_2d = info.type == ImageType::e2D;
    const auto& resolution = runtime->resolution;
    const u32 scaled_width = resolution.ScaleUp(info.size.width);
    const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
    std::unique_ptr<ImageView>& blit_view = scale_up ? scale_view : normal_view;
    std::optional<Framebuffer>& blit_framebuffer = scale_up ? scale_framebuffer : normal_framebuffer;
    if (!blit_view) {
        const auto view_info = ImageViewInfo(ImageViewType::e2D, info.format);
        blit_view = std::make_unique<ImageView>(*runtime, view_info, NULL_IMAGE_ID, *this);
    }

    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    const u32 src_width = (scale_up ? info.size.width : scaled_width) >> samples_x;
    const u32 src_height = (scale_up ? info.size.height : scaled_height) >> samples_y;
    const u32 dst_width = (scale_up ? scaled_width : info.size.width) >> samples_x;
    const u32 dst_height = (scale_up ? scaled_height : info.size.height) >> samples_y;
    const Region2D src_region{
        .start = {0, 0},
        .end = {s32(src_width), s32(src_height)},
    };
    const Region2D dst_region{
        .start = {0, 0},
        .end = {s32(dst_width), s32(dst_height)},
    };
    const VkExtent2D extent{
        .width = (std::max)(scaled_width, info.size.width) >> samples_x,
        .height = (std::max)(scaled_height, info.size.height) >> samples_y,
    };

    auto* view_ptr = blit_view.get();
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
        if (!blit_framebuffer)
            blit_framebuffer.emplace(*runtime, view_ptr, nullptr, extent, scale_up);
        if (info.num_samples > 1) {
            runtime->blit_image_helper.BlitColorMSAA(&*blit_framebuffer, *blit_view,
                dst_region, src_region);
        } else {
            runtime->blit_image_helper.BlitColor(&*blit_framebuffer, *blit_view,
                dst_region, src_region, operation, BLIT_OPERATION);
        }
    } else if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
        if (!blit_framebuffer)
            blit_framebuffer.emplace(*runtime, nullptr, view_ptr, extent, scale_up);
        const bool has_stencil = (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const bool can_blit_stencil =
            has_stencil && runtime->device.IsExtShaderStencilExportSupported();
        if (info.num_samples > 1) {
            runtime->blit_image_helper.BlitDepthStencilMSAA(&*blit_framebuffer, *blit_view,
                dst_region, src_region);
        } else if (can_blit_stencil) {
            runtime->blit_image_helper.BlitDepthStencil(&*blit_framebuffer, *blit_view,
                dst_region, src_region, operation, BLIT_OPERATION);
        } else {
            runtime->blit_image_helper.BlitDepth(&*blit_framebuffer, *blit_view, dst_region,
                                                 src_region);
        }
    } else {
        // TODO: Use helper blits where applicable
        flags &= ~ImageFlagBits::Rescaled;
        LOG_ERROR(Render_Vulkan, "Device does not support scaling format {}", info.format);
        return false;
    }
    return true;
}

bool Image::NeedsScaleHelper() const {
    const auto& device = runtime->device;
    if (info.num_samples > 1) {
        return true;
    }
    static constexpr auto OPTIMAL_FORMAT = FormatType::Optimal;
    const auto vk_format =
        MaxwellToVK::SurfaceFormat(device, OPTIMAL_FORMAT, false, info.format).format;
    const auto blit_usage = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool needs_blit_helper = !device.IsFormatSupported(vk_format, blit_usage, OPTIMAL_FORMAT);
    return needs_blit_helper;
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId image_id_, Image& image)
    : VideoCommon::ImageViewBase{info, image.info, image_id_, image.gpu_addr},
      device{&runtime.device}, image_handle{image.Handle()},
      samples(ConvertSampleCount(image.info.num_samples)) {
    using Shader::TextureType;

    const VkImageAspectFlags aspect_mask = ImageViewAspectMask(info);
    std::array<SwizzleSource, 4> swizzle{
        SwizzleSource::R,
        SwizzleSource::G,
        SwizzleSource::B,
        SwizzleSource::A,
    };
    if (!info.IsRenderTarget()) {
        swizzle = info.Swizzle();
        TryTransformSwizzleIfNeeded(format, swizzle,
                        device->MustEmulateBGR565(),
                        !device->IsExt4444FormatsSupported());
        if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
            std::ranges::transform(swizzle, swizzle.begin(), ConvertGreenRed);
            SanitizeDepthStencilSwizzle(swizzle, device->SupportsDepthStencilSwizzleOne());
        }
    }
    auto format_info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    if (device->ApiVersion() >= VK_API_VERSION_1_3) {
        const VkFormatProperties3 properties3 =
            device->GetPhysical().GetFormatProperties3(format_info.format);
        supports_minmax_filter = (properties3.optimalTilingFeatures &
                                  VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT) != 0;
    } else {
        supports_minmax_filter =
            (device->GetPhysical().GetFormatProperties(format_info.format).optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT) != 0;
    }
    requires_border_color_format = NeedsExplicitBorderColorFormat(format_info.format);
    swizzle_mapping = VkComponentMapping{
        .r = ComponentSwizzle(swizzle[0]),
        .g = ComponentSwizzle(swizzle[1]),
        .b = ComponentSwizzle(swizzle[2]),
        .a = ComponentSwizzle(swizzle[3]),
    };
    has_identity_swizzle = swizzle[0] == SwizzleSource::R && swizzle[1] == SwizzleSource::G &&
                           swizzle[2] == SwizzleSource::B && swizzle[3] == SwizzleSource::A;
    const VkImageUsageFlags requested_view_usage = ImageUsageFlags(format_info, format);
    const VkImageUsageFlags image_usage = image.UsageFlags();
    const VkImageUsageFlags clamped_view_usage = requested_view_usage & image_usage;
    const VkImageViewUsageCreateInfo image_view_usage{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = clamped_view_usage,
    };
    const VkImageViewASTCDecodeModeEXT astc_decode_mode{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT,
        .pNext = &image_view_usage,
        .decodeMode = VK_FORMAT_R8G8B8A8_UNORM,
    };
    const void* view_next = &image_view_usage;
    if (device->IsExtAstcDecodeModeSupported() && IsLdrAstcFormat(format_info.format)) {
        view_next = &astc_decode_mode;
    }
    auto subresource_range = MakeSubresourceRange(aspect_mask, info.range);
    if (True(flags & VideoCommon::ImageViewFlagBits::Slice)) {
        subresource_range.levelCount = 1;
    }
    const VkImageViewCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = view_next,
        .flags = 0,
        .image = image.Handle(),
        .viewType = VkImageViewType{},
        .format = format_info.format,
        .components = swizzle_mapping,
        .subresourceRange = subresource_range,
    };
    const auto create = [&](TextureType tex_type, std::optional<u32> num_layers) {
        VkImageViewCreateInfo ci{create_info};
        ci.viewType = ImageViewType(tex_type);
        if (num_layers) {
            ci.subresourceRange.layerCount = *num_layers;
        }
        vk::ImageView handle = device->GetLogical().CreateImageView(ci);
        if (device->HasDebuggingToolAttached()) {
            handle.SetObjectNameEXT(VideoCommon::Name(*this, gpu_addr).c_str());
        }
        image_views[static_cast<size_t>(tex_type)] = std::move(handle);
    };
    switch (info.type) {
    case VideoCommon::ImageViewType::e1D:
    case VideoCommon::ImageViewType::e1DArray:
        create(TextureType::Color1D, 1);
        create(TextureType::ColorArray1D, std::nullopt);
        render_target = Handle(TextureType::ColorArray1D);
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::e2DArray:
    case VideoCommon::ImageViewType::Rect:
        create(TextureType::Color2D, 1);
        create(TextureType::ColorArray2D, std::nullopt);
        render_target = Handle(Shader::TextureType::ColorArray2D);
        break;
    case VideoCommon::ImageViewType::e3D:
        create(TextureType::Color3D, std::nullopt);
        render_target = Handle(Shader::TextureType::Color3D);
        break;
    case VideoCommon::ImageViewType::Cube:
    case VideoCommon::ImageViewType::CubeArray:
        create(TextureType::Color2D, 1);
        create(TextureType::ColorCube, 6);
        create(TextureType::ColorArrayCube, std::nullopt);
        break;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT(false);
        break;
    }
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id_, Image& image, const SlotVector<Image>& slot_imgs)
    : ImageView{runtime, info, image_id_, image}
{
    slot_images = &slot_imgs;
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view_info, GPUVAddr gpu_addr_)
    : VideoCommon::ImageViewBase{info, view_info, gpu_addr_},
      buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, device{&runtime.device} {
    if (device->HasNullDescriptor()) {
        return;
    }

    // Handle fallback for devices without nullDescriptor
    ImageInfo info{};
    info.format = PixelFormat::A8B8G8R8_UNORM;

    null_image = MakeImage(*device, runtime.memory_allocator, info, {});
    image_handle = *null_image;
    for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; i++) {
        image_views[i] = MakeView(VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

ImageView::~ImageView() = default;

VkImageView ImageView::DepthView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (depth_view) {
        return *depth_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    depth_view = MakeView(info.format, VK_IMAGE_ASPECT_DEPTH_BIT);
    return *depth_view;
}

VkImageView ImageView::StencilView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (stencil_view) {
        return *stencil_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    stencil_view = MakeView(info.format, VK_IMAGE_ASPECT_STENCIL_BIT);
    return *stencil_view;
}

VkImageView ImageView::ColorView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (color_view) {
        return *color_view;
    }
    color_view = MakeView(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    return *color_view;
}

VkImageView ImageView::StorageView(Shader::TextureType texture_type,
                                   Shader::ImageFormat image_format) {
    if (image_handle) {
        if (image_format == Shader::ImageFormat::Typeless) {
            auto& view{typeless_storage_views[static_cast<size_t>(texture_type)]};
            if (!view) {
                auto info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
                view = MakeView(info.format, VK_IMAGE_ASPECT_COLOR_BIT, texture_type);
            }
            return *view;
        }
        const bool is_signed = image_format == Shader::ImageFormat::R8_SINT
            || image_format == Shader::ImageFormat::R16_SINT;
        if (!storage_views)
            storage_views.emplace();
        auto& views{is_signed ? storage_views->signeds : storage_views->unsigneds};
        auto& view{views[size_t(texture_type)]};
        if (!view)
            view = MakeView(Format(image_format), VK_IMAGE_ASPECT_COLOR_BIT, texture_type);
        return *view;
    }
    return VK_NULL_HANDLE;
}

bool ImageView::IsRescaled() const noexcept {
    return (*slot_images)[image_id].IsRescaled();
}

vk::ImageView ImageView::MakeView(VkFormat vk_format, VkImageAspectFlags aspect_mask,
                                  std::optional<Shader::TextureType> texture_type) {
    VkImageViewType view_type = ImageViewType(type);
    VkImageSubresourceRange subresource_range = MakeSubresourceRange(aspect_mask, range);
    if (texture_type) {
        view_type = ImageViewType(*texture_type);
        switch (view_type) {
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            break;
        default:
            subresource_range.layerCount = 1;
            break;
        }
    }
    return device->GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image_handle,
        .viewType = view_type,
        .format = vk_format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = subresource_range,
    });
}

CustomBorderColorBudget::~CustomBorderColorBudget() {
    Release();
}

CustomBorderColorBudget::CustomBorderColorBudget(CustomBorderColorBudget&& rhs) noexcept
    : device_ptr{std::exchange(rhs.device_ptr, nullptr)}, held{std::exchange(rhs.held, 0)} {}

CustomBorderColorBudget& CustomBorderColorBudget::operator=(
    CustomBorderColorBudget&& rhs) noexcept {
    if (this != &rhs) {
        Release();
        device_ptr = std::exchange(rhs.device_ptr, nullptr);
        held = std::exchange(rhs.held, 0);
    }
    return *this;
}

bool CustomBorderColorBudget::TryAcquire(const Device& device, size_t count) {
    if (!device.TryReserveCustomBorderColorSamplers(count)) {
        return false;
    }
    device_ptr = &device;
    held += count;
    return true;
}

void CustomBorderColorBudget::Release() noexcept {
    if (device_ptr != nullptr) {
        device_ptr->ReleaseCustomBorderColorSamplers(held);
    }
    device_ptr = nullptr;
    held = 0;
}

Sampler::Sampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& tsc) {
    const auto& device = runtime.device;
    device_ptr = &device;
    border_color = tsc.BorderColor();
    srgb_border_color = tsc.SrgbBorderColor();

    const f32 max_anisotropy = std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f);
    default_anisotropy = static_cast<f32>(1U << tsc.max_anisotropy);

    const VkFilter mag_filter{MaxwellToVK::Sampler::Filter(tsc.mag_filter)};
    const VkFilter min_filter{MaxwellToVK::Sampler::Filter(tsc.min_filter)};
    const VkSamplerMipmapMode mipmap_mode{MaxwellToVK::Sampler::MipmapMode(tsc.mipmap_filter)};

    const VkSamplerAddressMode wrap_u{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_u, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_v{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_v, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_p{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_p, tsc.mag_filter)};
    const bool samples_border = wrap_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_p == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

    reduction_mode = MaxwellToVK::SamplerReduction(tsc.reduction_filter);

    has_added_anisotropy = max_anisotropy > default_anisotropy;
    has_linear_filtering = mag_filter == VK_FILTER_LINEAR || min_filter == VK_FILTER_LINEAR ||
                           mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
    has_minmax_reduction = reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
    has_srgb_border_color = tsc.srgb_conversion != 0 && srgb_border_color != border_color;
    if (has_minmax_reduction && !device.IsExtSamplerFilterMinmaxSupported()) {
        LOG_WARNING(Render_Vulkan, "VK_EXT_sampler_filter_minmax is required");
        has_minmax_reduction = false;
    }
    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();
    needs_swizzle_mapping = has_custom_border_colors && device.NeedsBorderColorSwizzleMapping();

    if (has_custom_border_colors && GPU::Logging::IsActive()) {
        GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
            "VK_EXT_custom_border_color", "Sampler::Sampler");
    }
    if (device.IsExtBorderColorSwizzleSupported() && GPU::Logging::IsActive()) {
        GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
            "VK_EXT_border_color_swizzle", "Sampler::Sampler");
    }

    f32 min_lod = 0.0f;
    f32 max_lod = 0.25f;
    if (tsc.mipmap_filter != TextureMipmapFilter::None) {
        min_lod = tsc.MinLod();
        max_lod = tsc.MaxLod();
        // EDEN_NX2_FORCE_LOD0: sample only the top mip level (debug aid for mip layout problems).
        if (static const bool force_lod0 = std::getenv("EDEN_NX2_FORCE_LOD0") != nullptr; force_lod0) {
            min_lod = 0.0f;
            max_lod = 0.25f;
        }
    }
    base_ci = VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = mag_filter,
        .minFilter = min_filter,
        .mipmapMode = mipmap_mode,
        .addressModeU = wrap_u,
        .addressModeV = wrap_v,
        .addressModeW = wrap_p,
        .mipLodBias = tsc.LodBias(),
        .anisotropyEnable = static_cast<VkBool32>(max_anisotropy > 1.0f),
        .maxAnisotropy = max_anisotropy,
        .compareEnable = static_cast<VkBool32>(tsc.depth_compare_enabled),
        .compareOp = MaxwellToVK::Sampler::DepthCompareFunction(tsc.depth_compare_func),
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT,
        .unnormalizedCoordinates = VK_FALSE,
    };
    variants.reserve(1);
    Emplace(VariantKey{});
}

Sampler::VariantKey Sampler::MakeKey(const ImageView& image_view, bool is_depth) const noexcept {
    VariantKey key{};
    key.reduce_anisotropy = has_added_anisotropy && !image_view.SupportsAnisotropy();
    key.force_nearest = has_linear_filtering && IsPixelFormatInteger(image_view.format);
    key.drop_reduction = has_minmax_reduction && !image_view.SupportsMinmaxFilter();
    key.drop_custom_border = has_custom_border_colors && image_view.RequiresBorderColorFormat();
    key.srgb_border = has_srgb_border_color && IsPixelFormatSRGB(image_view.format);
    if (needs_swizzle_mapping && !key.drop_custom_border && !image_view.HasIdentitySwizzle()) {
        const VkComponentMapping& mapping = image_view.Swizzle();
        key.swizzle = {mapping.r, mapping.g, mapping.b, mapping.a};
    }
    return key;
}

VkSampler Sampler::Find(const VariantKey& key) const noexcept {
    const auto it = std::ranges::find(variants, key, &Variant::key);
    if (it == variants.end()) {
        return VK_NULL_HANDLE;
    }
    return *it->sampler;
}

VkSampler Sampler::Emplace(VariantKey key) {
    bool custom_border = has_custom_border_colors && !key.drop_custom_border;
    if (custom_border && !custom_border_color_budget.TryAcquire(*device_ptr, 1)) {
        static bool warned_budget = false;
        if (!warned_budget) {
            warned_budget = true;
        }
        custom_border = false;
        key.drop_custom_border = true;
        key.swizzle = {};
        if (const VkSampler existing = Find(key); existing != VK_NULL_HANDLE) {
            return existing;
        }
    }

    const f32 max_anisotropy = std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f);
    const f32 max_anisotropy_default = static_cast<f32>(1U << tsc.max_anisotropy);

    const VkFilter mag_filter{MaxwellToVK::Sampler::Filter(tsc.mag_filter)};
    const VkFilter min_filter{MaxwellToVK::Sampler::Filter(tsc.min_filter)};
    const VkSamplerMipmapMode mipmap_mode{MaxwellToVK::Sampler::MipmapMode(tsc.mipmap_filter)};
    const bool has_linear_filtering{mag_filter == VK_FILTER_LINEAR ||
                                    min_filter == VK_FILTER_LINEAR ||
                                    mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR};

    const VkSamplerAddressMode wrap_u{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_u, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_v{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_v, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_p{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_p, tsc.mag_filter)};
    const bool samples_border = wrap_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_p == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

    const VkSamplerReductionModeEXT reduction_mode =
        MaxwellToVK::SamplerReduction(tsc.reduction_filter);
    const bool has_minmax_reduction =
        device.IsExtSamplerFilterMinmaxSupported() &&
        reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;

    size_t custom_border_color_count = 1;
    if (max_anisotropy > max_anisotropy_default) {
        ++custom_border_color_count;
    }
    if (has_linear_filtering) {
        ++custom_border_color_count;
    }
    if (tsc.depth_compare_enabled) {
        ++custom_border_color_count;
    }
    if (has_minmax_reduction) {
        ++custom_border_color_count;
    }
    if (tsc.srgb_conversion && srgb_color != color) {
        ++custom_border_color_count;
    }

    const bool has_custom_border_colors =
        samples_border && device.IsCustomBorderColorUsable() &&
        custom_border_color_budget.TryAcquire(device, custom_border_color_count);

    const VkSamplerCustomBorderColorCreateInfoEXT border_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .pNext = nullptr,
        .customBorderColor = std::bit_cast<VkClearColorValue>(color),
        .format = VK_FORMAT_UNDEFINED,
    };
    const VkSamplerCustomBorderColorCreateInfoEXT srgb_border_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .pNext = nullptr,
        .customBorderColor = std::bit_cast<VkClearColorValue>(srgb_color),
        .format = VK_FORMAT_UNDEFINED,
    };
    const void* pnext = nullptr;
    const void* srgb_pnext = nullptr;
    if (has_custom_border_colors) {
        pnext = &border_ci;
        srgb_pnext = &srgb_border_ci;
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_custom_border_color", "Sampler::Sampler");
        }
    }
    if (device.IsExtBorderColorSwizzleSupported() && GPU::Logging::IsActive()) {
        GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
            "VK_EXT_border_color_swizzle", "Sampler::Sampler");
    }
    const VkSamplerReductionModeCreateInfoEXT reduction_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = pnext,
        .reductionMode = reduction_mode,
    };
    const VkSamplerReductionModeCreateInfoEXT reduction_ci_srgb{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = srgb_pnext,
        .reductionMode = reduction_mode,
    };
    const VkSamplerReductionModeCreateInfoEXT reduction_ci_without_border{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .reductionMode = reduction_mode,
    };
    const void* pnext_without_border = nullptr;
    if (device.IsExtSamplerFilterMinmaxSupported()) {
        pnext = &reduction_ci;
        srgb_pnext = &reduction_ci_srgb;
        pnext_without_border = &reduction_ci_without_border;
    } else if (reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT) {
        LOG_WARNING(Render_Vulkan, "VK_EXT_sampler_filter_minmax is required");
    }

    const auto make_create_info = [&](const f32 anisotropy, bool force_nearest,
                                      bool disable_compare, const void* chain,
                                      VkBorderColor fixed_border) {
        return VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = chain,
            .flags = 0,
            .magFilter = force_nearest ? VK_FILTER_NEAREST : mag_filter,
            .minFilter = force_nearest ? VK_FILTER_NEAREST : min_filter,
            .mipmapMode = force_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : mipmap_mode,
            .addressModeU = wrap_u,
            .addressModeV = wrap_v,
            .addressModeW = wrap_p,
            .mipLodBias = tsc.LodBias(),
            .anisotropyEnable =
                static_cast<VkBool32>(!force_nearest && anisotropy > 1.0f ? VK_TRUE : VK_FALSE),
            .maxAnisotropy = force_nearest ? 1.0f : anisotropy,
            .compareEnable = disable_compare ? VK_FALSE
                                             : static_cast<VkBool32>(tsc.depth_compare_enabled),
            .compareOp = MaxwellToVK::Sampler::DepthCompareFunction(tsc.depth_compare_func),
            .minLod = tsc.mipmap_filter == TextureMipmapFilter::None ? 0.0f : tsc.MinLod(),
            .maxLod = tsc.mipmap_filter == TextureMipmapFilter::None ? 0.25f : tsc.MaxLod(),
            .borderColor = fixed_border,
            .unnormalizedCoordinates = VK_FALSE,
        };
    };

    const auto create_sampler = [&](const f32 anisotropy, bool force_nearest,
                                    bool disable_compare = false,
                                    bool disable_custom_border = false,
                                    bool disable_minmax = false, bool use_srgb_border = false) {
        const bool custom_border = has_custom_border_colors && !disable_custom_border;
        const bool minmax = has_minmax_reduction && !disable_minmax;
        const void* chain = nullptr;
        if (custom_border && minmax) {
            chain = pnext;
            if (use_srgb_border) {
                chain = srgb_pnext;
            }
        } else if (minmax) {
            chain = pnext_without_border;
        } else if (custom_border) {
            chain = &border_ci;
            if (use_srgb_border) {
                chain = &srgb_border_ci;
            }
        }
        VkBorderColor fixed_border = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        if (!custom_border) {
            fixed_border = ConvertBorderColor(color);
            if (use_srgb_border) {
                fixed_border = ConvertBorderColor(srgb_color);
            }
        }
        return device.GetLogical().CreateSampler(
            make_create_info(anisotropy, force_nearest, disable_compare, chain, fixed_border));
    };

    sampler = create_sampler(max_anisotropy, false);

    if (max_anisotropy > max_anisotropy_default) {
        sampler_default_anisotropy = create_sampler(max_anisotropy_default, false);
    }
    if (has_linear_filtering) {
        // Integer-format image views can never be linearly filtered
        // (VUID-vkCmdDraw*-magFilter-04553); this sampler is cached purely from the guest's TSC,
        // decoupled from whichever ImageView it ends up paired with, so build a nearest-forced
        // fallback here for callers to swap to when the paired view turns out to be integer.
        sampler_nearest = create_sampler(1.0f, true);
    }
    if (const VkSampler existing = Find(key); existing != VK_NULL_HANDLE) {
        return existing;
    }
    if (has_custom_border_colors) {
        sampler_default_border = create_sampler(max_anisotropy, false, false, true, true);
    }
    if (has_minmax_reduction) {
        sampler_default_reduction = create_sampler(max_anisotropy, false, false, false, true);
    }
    if (tsc.srgb_conversion && srgb_color != color) {
        sampler_srgb_border =
            create_sampler(max_anisotropy, false, false, false, false, true);
    }
    needs_swizzle_mapping = has_custom_border_colors && device.NeedsBorderColorSwizzleMapping();
    if (needs_swizzle_mapping) {
        device_ptr = &device;
        border_color_value = color;
        srgb_border_color_value = srgb_color;
        swizzle_reduction_mode = reduction_mode;
        swizzle_uses_reduction = has_minmax_reduction;
        swizzle_base_ci = make_create_info(max_anisotropy, false, false, nullptr,
                                           VK_BORDER_COLOR_FLOAT_CUSTOM_EXT);
    }
}

VkSampler Sampler::HandleWithSwizzle(const VkComponentMapping& mapping, bool srgb) {
    const auto matches = [&](const SwizzleVariant& variant) {
        return variant.srgb == srgb && variant.mapping.r == mapping.r &&
               variant.mapping.g == mapping.g && variant.mapping.b == mapping.b &&
               variant.mapping.a == mapping.a;
    };
    const auto it = std::ranges::find_if(swizzle_variants, matches);
    if (it != swizzle_variants.end()) {
        return *it->sampler;
    }
    constexpr size_t MAX_SWIZZLE_VARIANTS = 8;
    if (swizzle_variants.size() >= MAX_SWIZZLE_VARIANTS ||
        !custom_border_color_budget.TryAcquire(*device_ptr, 1)) {
        return *sampler;
    }
    std::array<float, 4> value = border_color_value;
    if (srgb) {
        value = srgb_border_color_value;
    }
    const VkSamplerCustomBorderColorCreateInfoEXT border_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .pNext = nullptr,
        .customBorderColor = std::bit_cast<VkClearColorValue>(value),
        .format = VK_FORMAT_UNDEFINED,
    };
    const VkSamplerBorderColorComponentMappingCreateInfoEXT mapping_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,
        .pNext = &border_ci,
        .components = mapping,
        .srgb = VK_FALSE,
    };
    const VkSamplerReductionModeCreateInfoEXT reduction_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = &mapping_ci,
        .reductionMode = swizzle_reduction_mode,
    };
    VkSamplerCreateInfo create_info = swizzle_base_ci;
    create_info.pNext = &mapping_ci;
    if (swizzle_uses_reduction) {
        create_info.pNext = &reduction_ci;
    }
    swizzle_variants.push_back(SwizzleVariant{
        .mapping = mapping,
        .srgb = srgb,
        .sampler = device_ptr->GetLogical().CreateSampler(create_info),
    });
    return *swizzle_variants.back().sampler;
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : render_area{VkExtent2D{
          .width = key.size.width,
          .height = key.size.height,
      }} {
    CreateFramebuffer(runtime, color_buffers, depth_buffer, key.is_rescaled);
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, VkExtent2D extent, bool is_rescaled_)
    : render_area{extent} {
    std::array<ImageView*, NUM_RT> color_buffers{color_buffer};
    CreateFramebuffer(runtime, color_buffers, depth_buffer, is_rescaled_);
}

Framebuffer::~Framebuffer() = default;

/// Single-sampled color targets with a multisampled depth buffer, as used by target-independent
/// rasterization. Vulkan requires matching sample counts for these attachments.
static bool IsMixedSamplesTarget(std::span<ImageView*, NUM_RT> color_buffers,
                                 const ImageView* depth_buffer) {
    if (depth_buffer == nullptr || depth_buffer->Samples() == VK_SAMPLE_COUNT_1_BIT) {
        return false;
    }
    bool has_color = false;
    for (const ImageView* const color_buffer : color_buffers) {
        if (color_buffer == nullptr) {
            continue;
        }
        if (color_buffer->Samples() != VK_SAMPLE_COUNT_1_BIT) {
            return false;
        }
        has_color = true;
    }
    return has_color;
}

void Framebuffer::CreateFramebuffer(TextureCacheRuntime& runtime,
                                    std::span<ImageView*, NUM_RT> color_buffers,
                                    ImageView* depth_buffer, bool is_rescaled_) {
    std::array<PixelFormat, NUM_RT> color_formats{};
    std::array<VkImageView, NUM_RT> color_views{};
    PixelFormat depth_format = PixelFormat::Invalid;
    VkImageView depth_view = VK_NULL_HANDLE;
    s32 num_layers = 1;

    is_rescaled = is_rescaled_;
    const auto& resolution = runtime.resolution;

    u32 width = (std::numeric_limits<u32>::max)();
    u32 height = (std::numeric_limits<u32>::max)();
    for (size_t index = 0; index < NUM_RT; ++index) {
        const ImageView* const color_buffer = color_buffers[index];
        if (!color_buffer) {
            color_formats[index] = PixelFormat::Invalid;
            continue;
        }
        width = (std::min)(width, is_rescaled ? resolution.ScaleUp(color_buffer->size.width)
                                            : color_buffer->size.width);
        height = (std::min)(height, is_rescaled ? resolution.ScaleUp(color_buffer->size.height)
                                              : color_buffer->size.height);
        color_views[index] = color_buffer->RenderTarget();
        color_formats[index] = color_buffer->format;
        num_layers = (std::max)(num_layers, color_buffer->range.extent.layers);
        images[num_images] = color_buffer->ImageHandle();
        image_ranges[num_images] = MakeSubresourceRange(color_buffer);
        rt_map[index] = num_images;
        samples = color_buffer->Samples();
        color_attachments[index] = color_buffer->RenderTarget();
        color_attachment_formats[index] =
            MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal, true,
                                       color_buffer->format)
                .format;
        num_color_attachments = static_cast<u32>(index + 1);
        ++num_images;
    }
    const size_t num_colors = num_images;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageAspectFlags depth_aspect_mask = 0;
    if (depth_buffer) {
        width = (std::min)(width, is_rescaled ? resolution.ScaleUp(depth_buffer->size.width)
                                            : depth_buffer->size.width);
        height = (std::min)(height, is_rescaled ? resolution.ScaleUp(depth_buffer->size.height)
                                              : depth_buffer->size.height);
        depth_view = depth_buffer->RenderTarget();
        depth_format = depth_buffer->format;
        num_layers = (std::max)(num_layers, depth_buffer->range.extent.layers);
        images[num_images] = depth_buffer->ImageHandle();
        const VkImageSubresourceRange subresource_range = MakeSubresourceRange(depth_buffer);
        image_ranges[num_images] = subresource_range;
        samples = depth_buffer->Samples();
        depth_attachment = depth_buffer->RenderTarget();
        depth_attachment_format =
            MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal, true,
                                       depth_buffer->format)
                .format;
        ++num_images;
        has_depth = (subresource_range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        has_stencil = (subresource_range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        depth_image = depth_buffer->ImageHandle();
        depth_aspect_mask = subresource_range.aspectMask;
    }
    if (IsMixedSamplesTarget(color_buffers, depth_buffer)) {
        const VkExtent2D extent{(std::min)(render_area.width, width),
                                (std::min)(render_area.height, height)};
        CreateMixedSamplesFramebuffer(runtime, color_buffers, attachments, renderpass_key,
                                      static_cast<u32>((std::max)(num_layers, 1)), extent);
        return;
    }
    renderpass_key.samples = samples;
    const bool do_resolve_color = ENABLE_MSAA_TILER_RESOLVE &&
        samples != VK_SAMPLE_COUNT_1_BIT && num_colors > 0 && runtime.device.IsTiler();

    const bool do_resolve_depth_stencil = ENABLE_MSAA_TILER_RESOLVE &&
        samples != VK_SAMPLE_COUNT_1_BIT && depth_image != VK_NULL_HANDLE &&
        runtime.device.IsTiler() &&
        SupportsDepthStencilResolve(runtime.device, renderpass_key.depth_format);
    renderpass_key.resolve_depth_stencil = do_resolve_depth_stencil;

    discard_msaa_color =
        ENABLE_MSAA_RESOLVE_CONSUME && ENABLE_MSAA_COLOR_DISCARD && do_resolve_color;
    discard_msaa_depth_stencil = ENABLE_MSAA_RESOLVE_CONSUME &&
                                 ENABLE_MSAA_DEPTH_STENCIL_DISCARD && do_resolve_depth_stencil;

    UseRenderPass(runtime, renderpass_key);
    render_area.width = (std::min)(render_area.width, width);
    render_area.height = (std::min)(render_area.height, height);

    const u32 layers = static_cast<u32>((std::max)(num_layers, 1));
    formats = MakeRenderingFormats(runtime.device, color_formats, depth_format);
    attachments = MakeRenderingAttachments(formats, color_views, depth_view,
                                           VkRect2D{.offset = {0, 0}, .extent = render_area},
                                           layers);

    if (do_resolve_color) {
        for (size_t index = 0; index < NUM_RT; ++index) {
            if (color_formats[index] == PixelFormat::Invalid) {
                continue;
            }
            const VkImage msaa_image = images[rt_map[index]];
            VkRenderingAttachmentInfo& attachment = attachments.colors[index];
            attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            if (VideoCore::Surface::IsPixelFormatInteger(color_formats[index])) {
                attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            }
            attachment.resolveImageView = runtime.GetOrCreateResolveShadow(
                msaa_image, formats.colors[index], render_area, layers, VK_IMAGE_ASPECT_COLOR_BIT);
            resolve_shadow_images[num_resolve_shadows++] = msaa_image;
        }
    }

    if (do_resolve_depth_stencil) {
        const VkFormat vk_format =
            MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal, true, depth_format)
                .format;
        const VkImageView resolve_view = runtime.GetOrCreateResolveShadow(
            depth_image, vk_format, render_area, layers, depth_aspect_mask);
        const ResolveModes modes = PickResolveModes(runtime.device, depth_format);
        attachments.depth.resolveMode = modes.depth;
        attachments.depth.resolveImageView = resolve_view;
        attachments.stencil.resolveMode = modes.stencil;
        attachments.stencil.resolveImageView = resolve_view;
        resolve_shadow_images[num_resolve_shadows++] = depth_image;
    }

    num_color_buffers = static_cast<u32>(num_colors);
    CreateVkFramebuffer(runtime, attachments, static_cast<u32>((std::max)(num_layers, 1)));
}

void Framebuffer::CreateMixedSamplesFramebuffer(TextureCacheRuntime& runtime,
                                                std::span<ImageView*, NUM_RT> color_buffers,
                                                std::span<const VkImageView> attachments,
                                                RenderPassKey renderpass_key, u32 layers,
                                                VkExtent2D extent) {
    // attachments holds the real color targets followed by the depth buffer
    const size_t num_colors = attachments.size() - 1;
    boost::container::small_vector<VkImageView, NUM_RT * 2 + 2> mixed_attachments;
    for (const ImageView* const color_buffer : color_buffers) {
        if (!color_buffer) {
            continue;
        }
        const VkFormat vk_format = MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal,
                                                              true, color_buffer->format)
                                       .format;
        mixed_attachments.push_back(runtime.GetOrCreateTirColorScratch(
            color_buffer->ImageHandle(), vk_format, extent, layers, samples));
    }
    mixed_attachments.push_back(attachments[num_colors]);
    for (size_t index = 0; index < num_colors; ++index) {
        mixed_attachments.push_back(attachments[index]);
    }
    // Multisampled scratch color + depth, resolving color into the real single-sampled targets
    renderpass_key.samples = samples;
    renderpass_key.resolve_color = true;
    renderpass_key.resolve_depth_stencil = false;
    discard_msaa_color = false;
    discard_msaa_depth_stencil = false;
    UseRenderPass(runtime, renderpass_key);
    render_area = extent;
    num_color_buffers = static_cast<u32>(num_colors);
    CreateVkFramebuffer(runtime, mixed_attachments, layers);
}

void Framebuffer::UseRenderPass(TextureCacheRuntime& runtime,
                                const RenderPassKey& renderpass_key) {
    renderpass = runtime.render_pass_cache.Get(renderpass_key);
    render_pass_key = renderpass_key;
    render_pass_cache = &runtime.render_pass_cache;
    runtime_ptr = &runtime;
}

void Framebuffer::CreateVkFramebuffer(TextureCacheRuntime& runtime,
                                      std::span<const VkImageView> attachments, u32 layers) {
    framebuffer = runtime.device.GetLogical().CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = renderpass,
        .attachmentCount = static_cast<u32>(attachments.size()),
        .pAttachments = attachments.data(),
        .width = render_area.width,
        .height = render_area.height,
        .layers = layers,
    });
}

void Framebuffer::MarkResolveShadowsUpToDate() const {
    if (runtime_ptr == nullptr) {
        return;
    }
    for (u32 index = 0; index < num_resolve_shadows; ++index) {
        runtime_ptr->MarkResolveShadowUpToDate(resolve_shadow_images[index]);
    }
}

void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles) {
    const bool is_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        image.ScaleDown(true);
    }
    const bool reorder = !is_rescaled && True(image.flags & ImageFlagBits::ReorderableUpload);
    if (IsPixelFormatASTC(image.info.format)) {
        astc_decoder_pass->Assemble(image, map, swizzles, reorder);
    } else if (image.info.type == ImageType::e3D) {
        bl_unswizzle_3d_pass->Unswizzle(image, map, swizzles, reorder);
    } else if (image.info.type == ImageType::Linear) {
        pitch_unswizzle_pass->Unswizzle(image, map, swizzles, reorder);
    } else {
        bl_unswizzle_2d_pass->Unswizzle(image, map, swizzles, reorder);
    }
    if (is_rescaled) {
        image.ScaleUp();
    }
}

void TextureCacheRuntime::ImportGuestMemory(const Common::HostMemory& host_memory) {
    guest_memory.emplace(device, memory_allocator, scheduler, host_memory);
    if (guest_memory->Empty()) {
        guest_memory.reset();
    }
}

std::optional<StagingBufferRef> TextureCacheRuntime::GuestMemorySource(const u8* pointer,
                                                                       size_t size) const {
    if (pointer == nullptr) {
        return std::nullopt;
    }
    const std::optional<GuestMemory::Range> range = guest_memory->Find(pointer, size);
    if (!range || range->offset % device.GetStorageBufferAlignment() != 0) {
        return std::nullopt;
    }
    return StagingBufferRef{.buffer = range->buffer, .offset = range->offset};
}

void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    if (image.ExchangeInitialization()) {
        return;
    }
    if (!image.CanUploadHostMemory()) {
        RecordInitialLayout(scheduler, image.Handle(), image.AspectMask());
        return;
    }
    device.GetLogical().TransitionImageLayoutEXT(VkHostImageLayoutTransitionInfoEXT{
        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT,
        .pNext = nullptr,
        .image = image.Handle(),
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange{
            .aspectMask = image.AspectMask(),
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    });
}

} // namespace Vulkan
