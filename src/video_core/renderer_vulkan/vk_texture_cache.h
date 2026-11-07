// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bitset>
#include <span>

#include "video_core/texture_cache/texture_cache_base.h"

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_guest_memory.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Settings {
struct ResolutionScalingInfo;
}

namespace Vulkan {

using Common::SlotVector;
using VideoCommon::ImageId;
using VideoCommon::NUM_RT;
using VideoCommon::Region2D;
using VideoCore::Surface::PixelFormat;

class BlitImageHelper;
class DescriptorPool;
class Device;
class Image;
class ImageView;
class Framebuffer;
class StagingBufferPool;
class Scheduler;

struct ByteAddressedShadow {
    VkImage original{};
    VkImage packed{};
    VkImageView view{};
    VkBuffer bridge{};
    VkExtent2D extent{};
};

struct ByteAddressedRange {
    DAddr address{};
    u64 size{};

    [[nodiscard]] bool Overlaps(const ByteAddressedRange& other) const noexcept {
        return size != 0 && other.size != 0 &&
               (address < other.address ? other.address - address < size
                                        : address - other.address < other.size);
    }
};

[[nodiscard]] inline std::optional<VkExtent2D> PackedByteAddressedExtent(u32 width, u32 height) {
    if (width < 2 || (width & 1) != 0 || height == 0) {
        return std::nullopt;
    }
    return VkExtent2D{width / 2, height};
}

class TextureCacheRuntime {
public:
    explicit TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                 MemoryAllocator& memory_allocator_,
                                 StagingBufferPool& staging_buffer_pool_,
                                 BlitImageHelper& blit_image_helper_,
                                 DescriptorPool& descriptor_pool,
                                 ComputePassDescriptorQueue& compute_pass_descriptor_queue);

    void Finish();

    StagingBufferRef UploadStagingBuffer(size_t size);

    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void TickFrame();

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    u64 GetDeviceAllocationUsage() const;

    bool CanReportMemoryUsage() const;

    /// Images that did not fit the device memory budget so far (see MemoryAllocator).
    u32 MemoryPressureEvents() const noexcept {
        return memory_allocator.MemoryPressureEvents();
    }

    void LogMemoryStatistics() const;

    /// True when ASTC is re-compressed to BC3 and opaque images may use BC1 instead.
    bool UseOpaqueAstcFormat() const;

    bool CanDownloadMsaa(const VideoCommon::ImageInfo& info) const;

    [[nodiscard]] VkImage AcquireMsaaScratchImage(const VkImageCreateInfo& image_ci);

    void ReleaseMsaaScratchImage(VkImage image);

    void BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                   const Region2D& dst_region, const Region2D& src_region,
                   Tegra::Engines::Fermi2D::Filter filter,
                   Tegra::Engines::Fermi2D::Operation operation);

    /// Stands in for a DLSS evaluation: temporal upscale of input_color into dst, accumulated with
    /// a copy of dst's previous contents (the last output). False when dst cannot take it.
    bool TemporalUpscale(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& input_color,
                         ImageView& motion_vectors, std::array<f32, 2> jitter,
                         std::array<f32, 2> motion_scale);

    void CopyImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    void CopyImageMSAA(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    bool ShouldReinterpret(Image& dst, Image& src);

    void ReinterpretImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    [[nodiscard]] ByteAddressedShadow CreateByteAddressedShadow(const ImageView& view);

    void RetireByteAddressedShadow(const ByteAddressedShadow& shadow);

    static void CopyByteAddressedShadow(vk::CommandBuffer cmdbuf,
                                        const ByteAddressedShadow& shadow, bool unpack);

    void ConvertImage(Framebuffer* dst, ImageView& dst_view, ImageView& src_view);

    bool CanUploadMSAA() const noexcept {
        return true;
    }

    void AccelerateImageUpload(Image&, const StagingBufferRef&,
                               std::span<const VideoCommon::SwizzleParameters>);

    void ImportGuestMemory(const Common::HostMemory& host_memory);

    [[nodiscard]] bool HasGuestMemory() const noexcept {
        return guest_memory.has_value();
    }

    [[nodiscard]] std::optional<StagingBufferRef> GuestMemorySource(const u8* pointer,
                                                                    size_t size) const;

    void InsertUploadMemoryBarrier() {}

    void TransitionImageLayout(Image& image);

    bool HasBrokenTextureViewFormats() const noexcept {
        // No known Vulkan driver has broken image views
        return false;
    }

    bool HasNativeBgr() const noexcept {
        // All known Vulkan drivers can natively handle BGR textures
        return true;
    }

    [[nodiscard]] VkBuffer GetTemporaryBuffer(size_t needed_size);

    struct ResolveShadow {
        vk::Image image;
        vk::ImageView view;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        u32 layers = 0;
        VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool up_to_date = false;
    };

    [[nodiscard]] VkImageView GetOrCreateResolveShadow(VkImage msaa_image, VkFormat format,
                                                       VkExtent2D extent, u32 layers,
                                                       VkImageAspectFlags aspect_mask);

    [[nodiscard]] const ResolveShadow* GetValidResolveShadow(VkImage msaa_image) const;

    /// Multisampled stand-in for a single-sampled color target rendered with a TIR depth buffer
    [[nodiscard]] VkImageView GetOrCreateTirColorScratch(VkImage color_image, VkFormat format,
                                                         VkExtent2D extent, u32 layers,
                                                         VkSampleCountFlagBits samples);

    void InvalidateResolveShadow(VkImage msaa_image);

    void MarkResolveShadowUpToDate(VkImage msaa_image);

    void EraseResolveShadow(VkImage msaa_image);

    bool CopyResolveShadow(VkImage msaa_image, VkImage dst_image, const VkImageCopy2& region);

    std::span<const VkFormat> ViewFormats(PixelFormat format) {
        return view_formats[static_cast<std::size_t>(format)];
    }

    void BarrierFeedbackLoop();

    bool IsFormatDitherable(VideoCore::Surface::PixelFormat format);
    bool IsFormatScalable(VideoCore::Surface::PixelFormat format);

    VkFormat GetSupportedFormat(VkFormat requested_format, VkFormatFeatureFlags required_features) const;

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StagingBufferPool& staging_buffer_pool;
    BlitImageHelper& blit_image_helper;
    std::optional<ASTCDecoderPass> astc_decoder_pass;

    std::optional<BlockLinearUnswizzle3DPass> bl3d_unswizzle_pass;
    std::optional<BlockLinearUnswizzle2DBufferPass> bl2d_buffer_unswizzle_pass;
    const Settings::ResolutionScalingInfo& resolution;
    std::array<std::vector<VkFormat>, VideoCore::Surface::MaxPixelFormat> view_formats;
    std::bitset<VideoCore::Surface::MaxPixelFormat> host_copy_formats;

    static constexpr size_t indexing_slots = 8 * sizeof(size_t);
    std::array<vk::Buffer, indexing_slots> buffers{};
    struct MsaaScratchKey {
        VkFormat format;
        VkImageType type;
        u32 width;
        u32 height;
        u32 depth;
        u32 levels;
        u32 layers;
        VkImageUsageFlags usage;
        VkImageCreateFlags flags;

        bool operator==(const MsaaScratchKey&) const noexcept = default;
    };

    struct MsaaScratchImage {
        MsaaScratchKey key;
        vk::Image image;
        u64 tick;
        u32 unused_frames;
    };

    std::vector<MsaaScratchImage> msaa_scratch_images;
    ::Common::unordered_map<VkImage, ResolveShadow> resolve_shadows;
    struct TirColorScratch {
        vk::Image image;
        vk::ImageView view;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        u32 layers = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    };
    ::Common::unordered_map<VkImage, TirColorScratch> tir_color_scratch;
    std::vector<std::pair<u64, ResolveShadow>> pending_resolve_shadows;
    u64 framebuffer_serial{};
};

class Framebuffer {
public:
    explicit Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key);

    explicit Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, VkExtent2D extent, bool is_rescaled);

    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    Framebuffer(Framebuffer&&) = default;
    Framebuffer& operator=(Framebuffer&&) = default;

    void CreateFramebuffer(TextureCacheRuntime& runtime,
                           std::span<ImageView*, NUM_RT> color_buffers, ImageView* depth_buffer,
                           bool is_rescaled = false);

    [[nodiscard]] u64 Id() const noexcept {
        return id;
    }

    [[nodiscard]] const RenderingFormats& Formats() const noexcept {
        return formats;
    }

    [[nodiscard]] const RenderingAttachments& Attachments() const noexcept {
        return attachments;
    }

    [[nodiscard]] VkExtent2D RenderArea() const noexcept {
        return render_area;
    }

    [[nodiscard]] u32 Layers() const noexcept {
        return layers;
    }

    [[nodiscard]] VkSampleCountFlagBits Samples() const noexcept {
        return samples;
    }

    [[nodiscard]] u32 NumImages() const noexcept {
        return num_images;
    }

    [[nodiscard]] const std::array<VkImage, 9>& Images() const noexcept {
        return images;
    }

    [[nodiscard]] const std::array<VkImageSubresourceRange, 9>& ImageRanges() const noexcept {
        return image_ranges;
    }

    [[nodiscard]] bool HasAspectColorBit(size_t index) const noexcept {
        return (image_ranges.at(rt_map[index]).aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    }

    [[nodiscard]] bool HasAspectDepthBit() const noexcept {
        return has_depth;
    }

    [[nodiscard]] bool HasAspectStencilBit() const noexcept {
        return has_stencil;
    }

    [[nodiscard]] bool IsRescaled() const noexcept {
        return is_rescaled;
    }

    /// Records that a render pass has begun, so its resolve attachments will hold valid contents
    /// once it ends.
    void MarkResolveShadowsUpToDate() const;

    [[nodiscard]] bool DiscardsMsaaDepth() const noexcept {
        return discard_msaa_depth;
    }

    [[nodiscard]] bool DiscardsMsaaDepthStencil() const noexcept {
        return discard_msaa_depth_stencil;
    }

    /// Records that a render pass has begun, so its resolve attachments will hold valid contents
    /// once it ends.
    void MarkResolveShadowsUpToDate() const;

private:
    /// Builds a framebuffer for single-sampled color targets drawn with a multisampled depth
    /// buffer (target-independent rasterization). Color is rendered into multisampled scratch
    /// images that resolve into the real targets at the end of the pass.
    void CreateMixedSamplesFramebuffer(TextureCacheRuntime& runtime,
                                       std::span<ImageView*, NUM_RT> color_buffers,
                                       std::span<const VkImageView> attachments,
                                       RenderPassKey renderpass_key, u32 layers,
                                       VkExtent2D extent);

    /// Fetches the render pass for the key from the cache and remembers it for variants.
    void UseRenderPass(TextureCacheRuntime& runtime, const RenderPassKey& renderpass_key);

    /// Creates the Vulkan framebuffer for the current render pass and render area.
    void CreateVkFramebuffer(TextureCacheRuntime& runtime,
                             std::span<const VkImageView> attachments, u32 layers);

    static constexpr size_t NUM_MEMOIZED_RENDER_PASS_VARIANTS = 8;

    vk::Framebuffer framebuffer;
    VkRenderPass renderpass{};
    VkExtent2D render_area{};
    u32 layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 num_images = 0;
    std::array<VkImage, 9> images{};
    std::array<VkImageSubresourceRange, 9> image_ranges{};
    std::array<size_t, NUM_RT> rt_map{};
    std::array<VkImageView, NUM_RT> color_attachments{};
    std::array<VkFormat, NUM_RT> color_attachment_formats{};
    std::array<VkImageView, NUM_RT> color_resolve_attachments{};
    std::array<VkResolveModeFlagBits, NUM_RT> color_resolve_modes{};
    VkImageView depth_attachment{};
    VkImageView depth_resolve_attachment{};
    VkResolveModeFlagBits depth_resolve_mode = VK_RESOLVE_MODE_NONE;
    VkResolveModeFlagBits stencil_resolve_mode = VK_RESOLVE_MODE_NONE;
    VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
    u32 num_color_attachments = 0;
    u32 layer_count = 1;
    bool has_depth{};
    bool has_stencil{};
    bool is_rescaled{};
    std::array<VkImage, 9> resolve_shadow_images{};
    u32 num_resolve_shadows = 0;
    TextureCacheRuntime* runtime_ptr{nullptr};
};

class Image : public VideoCommon::ImageBase {
public:
    explicit Image(TextureCacheRuntime&, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr,
                   VAddr cpu_addr);
    explicit Image(const VideoCommon::NullImageParams&);

    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    void UploadMemory(VkBuffer buffer, VkDeviceSize offset,
                      std::span<const VideoCommon::BufferImageCopy> copies);

    void UploadMemory(const StagingBufferRef& map,
                      std::span<const VideoCommon::BufferImageCopy> copies);

    /// Replaces the image contents with its block linear guest data held in a GPU buffer.
    /// Returns false when the image layout is not supported.
    bool UploadSwizzledFromBuffer(VkBuffer buffer, VkDeviceSize offset);

    void DownloadMemory(VkBuffer buffer, size_t offset,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    void DownloadMemory(std::span<VkBuffer> buffers, std::span<size_t> offsets,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    void DownloadMemory(const StagingBufferRef& map,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    [[nodiscard]] VkImage Handle() const noexcept {
        return *(this->*current_image);
    }

    [[nodiscard]] VkImageAspectFlags AspectMask() const noexcept {
        return aspect_mask;
    }

    [[nodiscard]] VkImageUsageFlags UsageFlags() const noexcept {
        return (this->*current_image).UsageFlags();
    }

    /// Where the image's memory lies, for the collector (TextureCache::RunGarbageCollector).
    [[nodiscard]] vk::Image::MemoryBlock MemoryBlock() const noexcept {
        return original_image.GetMemoryBlock();
    }

    /// Returns true when the image is already initialized and mark it as initialized
    [[nodiscard]] bool ExchangeInitialization() noexcept {
        return std::exchange(InitializationFor(current_image), true);
    }

    VkImageView StorageImageView(s32 level) noexcept;

    bool IsRescaled() const noexcept;

    bool ScaleUp(bool ignore = false);

    bool ScaleDown(bool ignore = false);

private:
    bool BlitScaleHelper(bool scale_up);

    bool NeedsScaleHelper() const;

    std::vector<vk::ImageView>& StorageViewsFor(vk::Image Image::*image);

    bool& InitializationFor(vk::Image Image::*image) noexcept;

    Scheduler* scheduler{};
    TextureCacheRuntime* runtime{};

    vk::Image original_image;
    vk::Image scaled_image;

    // Use a pointer to field because it is relative, so that the object can be
    // moved without breaking the reference.
    vk::Image Image::*current_image{};

    std::vector<vk::ImageView> storage_image_views;
    std::vector<vk::ImageView> scaled_storage_image_views;
    VkImageAspectFlags aspect_mask = 0;
    bool original_initialized = false;
    bool scaled_initialized = false;

    std::optional<Framebuffer> scale_framebuffer;
    std::optional<Framebuffer> normal_framebuffer;
    std::unique_ptr<ImageView> scale_view;
    std::unique_ptr<ImageView> normal_view;
};

class ImageView : public VideoCommon::ImageViewBase {
public:
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, ImageId, Image&);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, ImageId, Image&,
                       const SlotVector<Image>&);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
                       const VideoCommon::ImageViewInfo&, GPUVAddr);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams&);

    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    ImageView(ImageView&&) = default;
    ImageView& operator=(ImageView&&) = default;

    [[nodiscard]] VkImageView DepthView();

    [[nodiscard]] VkImageView StencilView();

    [[nodiscard]] VkImageView ColorView();

    [[nodiscard]] VkImageView StorageView(Shader::TextureType texture_type,
                                          Shader::ImageFormat image_format, bool is_byte_addressed);

    [[nodiscard]] bool IsRescaled() const noexcept;

    [[nodiscard]] VkImageView Handle(Shader::TextureType texture_type) const noexcept {
        return *image_views[static_cast<size_t>(texture_type)];
    }

    [[nodiscard]] VkImage ImageHandle() const noexcept {
        return image_handle;
    }

    [[nodiscard]] const Image* BackingImage() const noexcept {
        return slot_images && image_id ? &(*slot_images)[image_id] : nullptr;
    }

    [[nodiscard]] VkImageView RenderTarget() const noexcept {
        return render_target;
    }

    [[nodiscard]] VkSampleCountFlagBits Samples() const noexcept {
        return samples;
    }

    [[nodiscard]] bool RequiresBorderColorFormat() const noexcept {
        return requires_border_color_format;
    }

    [[nodiscard]] bool SupportsMinmaxFilter() const noexcept {
        return supports_minmax_filter;
    }

    [[nodiscard]] const VkComponentMapping& Swizzle() const noexcept {
        return swizzle_mapping;
    }

    [[nodiscard]] bool HasIdentitySwizzle() const noexcept {
        return has_identity_swizzle;
    }

    [[nodiscard]] GPUVAddr GpuAddr() const noexcept {
        return gpu_addr;
    }

    [[nodiscard]] u32 BufferSize() const noexcept {
        return buffer_size;
    }

private:
    struct StorageViews {
        std::array<std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES>,
                   static_cast<size_t>(Shader::ImageFormat::R32G32B32A32_UINT) + 1> formats;
    };

    [[nodiscard]] vk::ImageView MakeView(VkFormat vk_format, VkImageAspectFlags aspect_mask,
                                         std::optional<Shader::TextureType> texture_type = std::nullopt);

    const Device* device = nullptr;
    const SlotVector<Image>* slot_images = nullptr;

    std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES> image_views;
    std::optional<StorageViews> storage_views;
    std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES> typeless_storage_views;
    vk::ImageView depth_view;
    vk::ImageView stencil_view;
    vk::ImageView color_view;
    vk::Image null_image;
    VkImage image_handle = VK_NULL_HANDLE;
    VkImageView render_target = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 buffer_size = 0;

    VkComponentMapping swizzle_mapping{};

    bool requires_border_color_format = false;
    bool supports_minmax_filter = false;
    bool has_identity_swizzle = true;
};

class ImageAlloc : public VideoCommon::ImageAllocBase {};

class CustomBorderColorBudget {
public:
    CustomBorderColorBudget() = default;
    ~CustomBorderColorBudget();

    CustomBorderColorBudget(const CustomBorderColorBudget&) = delete;
    CustomBorderColorBudget& operator=(const CustomBorderColorBudget&) = delete;

    CustomBorderColorBudget(CustomBorderColorBudget&& rhs) noexcept;
    CustomBorderColorBudget& operator=(CustomBorderColorBudget&& rhs) noexcept;

    bool TryAcquire(const Device& device, size_t count);

private:
    void Release() noexcept;

    const Device* device_ptr = nullptr;
    size_t held = 0;
};

class Sampler {
public:
    explicit Sampler(TextureCacheRuntime&, const Tegra::Texture::TSCEntry&);

    [[nodiscard]] VkSampler Handle() const noexcept {
        return *variants.front().sampler;
    }

    [[nodiscard]] VkSampler HandleFor(const ImageView& image_view, bool is_depth);

    /// The guest descriptor and the host state built from it, for EDEN_NX2_FRAME_DUMP_SAMPLED.
    [[nodiscard]] const Tegra::Texture::TSCEntry& Descriptor() const noexcept {
        return descriptor;
    }

    [[nodiscard]] const VkSamplerCreateInfo& CreateInfo() const noexcept {
        return base_ci;
    }

    [[nodiscard]] VkSampler HandleWithNearestFilter() const noexcept {
        return *sampler_nearest;
    }

    [[nodiscard]] bool HasLinearFiltering() const noexcept {
        return static_cast<bool>(sampler_nearest);
    }

    [[nodiscard]] VkSampler HandleWithoutDepthComparison() const noexcept {
        return *sampler_noncompare;
    }

    [[nodiscard]] bool HasDepthComparison() const noexcept {
        return static_cast<bool>(sampler_noncompare);
    }

    [[nodiscard]] VkSampler HandleWithDefaultBorderColor() const noexcept {
        return *sampler_default_border;
    }

    [[nodiscard]] bool HasCustomBorderColor() const noexcept {
        return static_cast<bool>(sampler_default_border);
    }

    [[nodiscard]] VkSampler HandleWithDefaultReduction() const noexcept {
        return *sampler_default_reduction;
    }

    [[nodiscard]] bool HasMinmaxReduction() const noexcept {
        return static_cast<bool>(sampler_default_reduction);
    }

    [[nodiscard]] VkSampler HandleWithSrgbBorderColor() const noexcept {
        return *sampler_srgb_border;
    }

    [[nodiscard]] bool HasSrgbBorderColor() const noexcept {
        return static_cast<bool>(sampler_srgb_border);
    }

    [[nodiscard]] bool NeedsSwizzleMapping() const noexcept {
        return needs_swizzle_mapping;
    }

    [[nodiscard]] VkSampler HandleWithSwizzle(const VkComponentMapping& mapping, bool srgb);

private:
    struct SwizzleVariant {
        VkComponentMapping mapping;
        bool srgb;
        vk::Sampler sampler;
    };

    CustomBorderColorBudget custom_border_color_budget;

    vk::Sampler sampler;
    vk::Sampler sampler_default_anisotropy;
    vk::Sampler sampler_nearest;
    vk::Sampler sampler_noncompare;
    vk::Sampler sampler_default_border;
    vk::Sampler sampler_default_reduction;
    vk::Sampler sampler_srgb_border;

    CustomBorderColorBudget custom_border_color_budget;
    std::vector<Variant> variants;

    const Device* device_ptr{nullptr};
    VkSamplerCreateInfo base_ci{};
    VkSamplerReductionModeEXT reduction_mode{VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT};
    std::array<float, 4> border_color{};
    std::array<float, 4> srgb_border_color{};
    f32 default_anisotropy{1.0f};

    bool has_added_anisotropy{};
    bool has_linear_filtering{};
    bool has_minmax_reduction{};
    bool has_custom_border_colors{};
    bool has_srgb_border_color{};
    bool needs_swizzle_mapping{};
};

struct TextureCacheParams {
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = true;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;
    static constexpr bool HAS_MSAA_DOWNLOADS = true;
    static constexpr bool USE_UNIFIED_MEMORY = true;

    using Runtime = Vulkan::TextureCacheRuntime;
    using Image = Vulkan::Image;
    using ImageAlloc = Vulkan::ImageAlloc;
    using ImageView = Vulkan::ImageView;
    using Sampler = Vulkan::Sampler;
    using Framebuffer = Vulkan::Framebuffer;
    using AsyncBuffer = Vulkan::StagingBufferRef;
    using BufferType = VkBuffer;
};

using TextureCache = VideoCommon::TextureCache<TextureCacheParams>;

} // namespace Vulkan
