// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/image/image.h"

#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "kernel/memory.h"

#include <algorithm>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

[[nodiscard]] vk::ImageType HostImageType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1D: return vk::ImageType::e1D;
		case Prospero::ImageType::kColor3D: return vk::ImageType::e3D;
		case Prospero::ImageType::kColor2D: return vk::ImageType::e2D;
		default: EXIT("non-base image type: %u\n", static_cast<uint32_t>(type));
	}
}

[[nodiscard]] vk::ImageCreateFlags
ImageCreateFlags(const GraphicContext& graphics, const ImageInfo& info, vk::ImageUsageFlags usage) {
	vk::ImageCreateFlags flags {};
	if (DepthAspectTransferFormat(info.pixel_format) == vk::Format::eUndefined) {
		flags |= vk::ImageCreateFlagBits::eMutableFormat;
		if (info.IsBlock() && graphics.supports_block_texel_view) {
			flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
		}
	}
	if (usage & vk::ImageUsageFlagBits::eStorage) {
		flags |= vk::ImageCreateFlagBits::eExtendedUsage;
	}
	if (info.IsVolume()) {
		flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
	}
	return flags;
}

[[nodiscard]] bool HasFormatFeature(vk::FormatProperties      properties,
                                    vk::FormatFeatureFlagBits feature) {
	return static_cast<bool>(properties.optimalTilingFeatures & feature);
}

[[nodiscard]] vk::ImageUsageFlags ImageUsageFlags(GraphicContext& graphics, const ImageInfo& info) {
	auto usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	if (info.IsBlock()) {
		usage |= vk::ImageUsageFlagBits::eSampled;
		if (graphics.supports_block_texel_view) {
			const auto storage = usage | vk::ImageUsageFlagBits::eStorage;
			if (graphics.GetImageFormatProperties(info.pixel_format, HostImageType(info.type),
			                                      vk::ImageTiling::eOptimal, storage,
			                                      ImageCreateFlags(graphics, info, storage),
			                                      nullptr) == vk::Result::eSuccess) {
				usage = storage;
			} else {
				static std::atomic_flag warned = ATOMIC_FLAG_INIT;
				if (!warned.test_and_set(std::memory_order_relaxed)) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "Warning: format {} does not support storage access; block-compressed "
					    "textures written by the guest will not render.\n",
					    vk::to_string(info.pixel_format)));
				}
			}
		}
		return usage;
	}
	const auto properties = graphics.GetFormatProperties(info.pixel_format);
	if (HasFormatFeature(properties, vk::FormatFeatureFlagBits::eSampledImage)) {
		usage |= vk::ImageUsageFlagBits::eSampled;
	}
	if (DepthAspectTransferFormat(info.pixel_format) != vk::Format::eUndefined) {
		usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
		if (graphics.attachment_feedback_loop_enabled && (usage & vk::ImageUsageFlagBits::eSampled)) {
			usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
		}
		return usage;
	}
	if (HasFormatFeature(properties, vk::FormatFeatureFlagBits::eColorAttachment)) {
		usage |= vk::ImageUsageFlagBits::eColorAttachment;
	}
	if (info.samples == 1) {
		usage |= vk::ImageUsageFlagBits::eStorage;
	}
	return usage;
}

void ValidateOptionalRange(GuestRange range, const char* name) {
	if (!range.ValidOrEmpty()) {
		EXIT("invalid %s image range: address=0x%016llx size=0x%016llx\n", name,
		     static_cast<unsigned long long>(range.address),
		     static_cast<unsigned long long>(range.size));
	}
}

[[nodiscard]] std::unique_ptr<VulkanImage> CreateTransferImage(GraphicContext& graphics,
                                                               const VulkanImage& like,
                                                               vk::Extent3D       extent) {
	vk::ImageCreateInfo create {};
	create.imageType     = like.image_type;
	create.extent        = extent;
	create.mipLevels     = like.mip_levels;
	create.arrayLayers   = like.layers;
	create.format        = like.format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	create.samples       = vk::SampleCountFlagBits::e1;
	auto image           = std::make_unique<VulkanImage>();
	if (!graphics.CreateImage(create, *image)) {
		EXIT("failed to create a resampling image %ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(like.format));
	}
	return image;
}

void TransferImageBarrier(vk::CommandBuffer command, vk::Image image, vk::ImageAspectFlags aspect,
                          vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags2 src_access,
                          vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = src_access;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllTransfer;
	barrier.dstAccessMask       = dst_access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

// Resamples every level/layer of `source` (TransferSrcOptimal) into `destination`
// (TransferDstOptimal).
void BlitLevels(GraphicContext& graphics, vk::CommandBuffer command, vk::Image source,
                vk::Extent3D source_extent, vk::Image destination, vk::Extent3D destination_extent,
                vk::Format format, vk::ImageAspectFlags aspect, uint32_t levels, uint32_t layers) {
	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto filter   = aspect == vk::ImageAspectFlagBits::eColor &&
	                            (features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)
	                          ? vk::Filter::eLinear
	                          : vk::Filter::eNearest;
	std::vector<vk::ImageBlit> regions;
	regions.reserve(levels);
	for (uint32_t level = 0; level < levels; level++) {
		vk::ImageBlit region {};
		region.srcSubresource = {aspect, level, 0, layers};
		region.dstSubresource = {aspect, level, 0, layers};
		region.srcOffsets[1]  = vk::Offset3D {
		     static_cast<int32_t>(std::max(source_extent.width >> level, 1u)),
		     static_cast<int32_t>(std::max(source_extent.height >> level, 1u)),
		     static_cast<int32_t>(std::max(source_extent.depth >> level, 1u))};
		region.dstOffsets[1] = vk::Offset3D {
		    static_cast<int32_t>(std::max(destination_extent.width >> level, 1u)),
		    static_cast<int32_t>(std::max(destination_extent.height >> level, 1u)),
		    static_cast<int32_t>(std::max(destination_extent.depth >> level, 1u))};
		regions.push_back(region);
	}
	command.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, destination,
	                  vk::ImageLayout::eTransferDstOptimal, static_cast<uint32_t>(regions.size()),
	                  regions.data(), filter);
}

} // namespace

vk::Extent3D Image::ScaledExtent(vk::Extent3D extent, float scale) noexcept {
	if (scale >= 1.0f) {
		return extent;
	}
	return {std::max(1u, static_cast<uint32_t>(static_cast<float>(extent.width) * scale + 0.5f)),
	        std::max(1u, static_cast<uint32_t>(static_cast<float>(extent.height) * scale + 0.5f)),
	        extent.depth};
}

vk::ImageAspectFlags Image::FullAspectMask(vk::Format format) noexcept {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eX8D24UnormPack32:
		case vk::Format::eD32Sfloat: return vk::ImageAspectFlagBits::eDepth;
		case vk::Format::eS8Uint: return vk::ImageAspectFlagBits::eStencil;
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		default: return vk::ImageAspectFlagBits::eColor;
	}
}

Image::Barriers Image::GetBarriers(vk::ImageLayout                      destination_layout,
                                   vk::AccessFlags2                     destination_access,
                                   vk::PipelineStageFlags2              destination_stage,
                                   std::optional<ImageSubresourceRange> range) {
	auto& state              = backing.state;
	auto& subresource_states = backing.subresource_states;
	if (range && info.IsVolume()) {
		range->base_layer  = 0;
		range->layer_count = 1;
	}

	const bool partial =
	    range && (range->base_level != 0 || range->level_count != info.resources.levels ||
	              range->base_layer != 0 || range->layer_count != info.resources.layers);
	const bool has_subresource_states = !subresource_states.empty();

	// Inside a render pass, an attachment that draws also sample (read-only depth, feedback
	// loops) alternates between attachment access and attachment plus shader read without a
	// layout change. A barrier there only ends the pass (Stray split its 4K passes per draw), so
	// the accesses are merged and the barrier is left to the next real transition.
	// `setprop debug.capy.merge_access 0` emits the barriers as before.
	static Common::DebugFlags::CachedValue merge_access("debug.capy.merge_access", 1);
	constexpr auto in_pass_access = vk::AccessFlagBits2::eColorAttachmentRead |
	                                vk::AccessFlagBits2::eColorAttachmentWrite |
	                                vk::AccessFlagBits2::eDepthStencilAttachmentRead |
	                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
	                                vk::AccessFlagBits2::eShaderRead;
	const bool may_merge = merge_access.Get() != 0 && binding.is_target && m_scheduler.Active() &&
	                       m_scheduler.Current().IsRendering() &&
	                       !(destination_access & ~vk::AccessFlags2 {in_pass_access});
	const auto merges    = [&](const VulkanImageState& current) {
		return may_merge && current.layout == destination_layout &&
		       !(current.access_mask & ~vk::AccessFlags2 {in_pass_access});
	};

	Barriers barriers;
	if (partial || has_subresource_states) {
		if (!has_subresource_states) {
			subresource_states.resize(info.resources.levels * info.resources.layers, state);
		}

		const uint32_t base_level  = partial ? range->base_level : 0;
		const uint32_t level_count = partial ? range->level_count : info.resources.levels;
		const uint32_t base_layer  = partial ? range->base_layer : 0;
		const uint32_t layer_count = partial ? range->layer_count : info.resources.layers;
		for (uint32_t level = base_level; level < base_level + level_count; level++) {
			for (uint32_t layer = base_layer; layer < base_layer + layer_count; layer++) {
				const auto index = level * info.resources.layers + layer;
				EXIT_IF(index >= subresource_states.size());
				auto& subresource_state = subresource_states[index];
				if (merges(subresource_state)) {
					subresource_state.access_mask |= destination_access;
					subresource_state.pl_stage |= destination_stage;
					continue;
				}

				constexpr auto write_access = vk::AccessFlagBits2::eTransferWrite |
				                              vk::AccessFlagBits2::eShaderWrite |
				                              vk::AccessFlagBits2::eMemoryWrite;
				const bool     repeated_write =
				    static_cast<bool>(subresource_state.access_mask & write_access);
				if (subresource_state.layout != destination_layout ||
				    subresource_state.access_mask != destination_access || repeated_write) {
					vk::ImageMemoryBarrier2 barrier {};
					barrier.srcStageMask                    = subresource_state.pl_stage;
					barrier.srcAccessMask                   = subresource_state.access_mask;
					barrier.dstStageMask                    = destination_stage;
					barrier.dstAccessMask                   = destination_access;
					barrier.oldLayout                       = subresource_state.layout;
					barrier.newLayout                       = destination_layout;
					barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
					barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
					barrier.image                           = backing.image;
					barrier.subresourceRange.aspectMask     = FullAspectMask(backing.format);
					barrier.subresourceRange.baseMipLevel   = level;
					barrier.subresourceRange.levelCount     = 1;
					barrier.subresourceRange.baseArrayLayer = layer;
					barrier.subresourceRange.layerCount     = 1;
					barriers.push_back(barrier);
					subresource_state = {destination_stage, destination_access, destination_layout};
				}
			}
		}

		if (!partial) {
			subresource_states.clear();
		}
	} else {
		constexpr auto write_access   = vk::AccessFlagBits2::eTransferWrite |
		                                vk::AccessFlagBits2::eShaderWrite |
		                                vk::AccessFlagBits2::eMemoryWrite;
		const bool     repeated_write = static_cast<bool>(state.access_mask & write_access);
		if (merges(state)) {
			state.access_mask |= destination_access;
			state.pl_stage |= destination_stage;
			return {};
		}
		if (state.layout == destination_layout && state.access_mask == destination_access &&
		    !repeated_write) {
			return {};
		}

		vk::ImageMemoryBarrier2 barrier {};
		barrier.srcStageMask                    = state.pl_stage;
		barrier.srcAccessMask                   = state.access_mask;
		barrier.dstStageMask                    = destination_stage;
		barrier.dstAccessMask                   = destination_access;
		barrier.oldLayout                       = state.layout;
		barrier.newLayout                       = destination_layout;
		barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.image                           = backing.image;
		barrier.subresourceRange.aspectMask     = FullAspectMask(backing.format);
		barrier.subresourceRange.baseMipLevel   = 0;
		barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
		barriers.push_back(barrier);
	}

	state = {destination_stage, destination_access, destination_layout};
	return barriers;
}

void Image::Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
                    std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer) {
	const auto transfer_access =
	    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
	vk::PipelineStageFlags2 destination_stage {};
	if (static_cast<bool>(destination_access & transfer_access)) {
		destination_stage |= vk::PipelineStageFlagBits2::eTransfer;
	}
	if (!destination_access ||
	    static_cast<bool>(destination_access & ~vk::AccessFlags2 {transfer_access})) {
		destination_stage |=
		    vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
	}
	const auto barriers =
	    GetBarriers(destination_layout, destination_access, destination_stage, range);
	if (barriers.empty()) {
		return;
	}
	// `setprop debug.capy.transit_log N`: log the next N transitions that end a render pass, with
	// their layouts and caller (tag CapyPS5-Transit). Change N to log again.
	static Common::DebugFlags::CachedValue transit_log("debug.capy.transit_log", 0);
	static int64_t                         transit_armed = 0;
	static int64_t                         transit_left  = 0;
	if (transit_log.Get() != transit_armed) {
		transit_armed = transit_log.Get();
		transit_left  = transit_armed;
	}
#if defined(__ANDROID__)
	if (transit_left > 0 && m_scheduler.Active() && m_scheduler.Current().IsRendering()) {
		transit_left--;
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Transit",
		                    "end pass: addr=0x%llx %ux%u layout %u -> %u access 0x%llx -> 0x%llx caller %p",
		                    static_cast<unsigned long long>(track_addr), backing.extent.width, backing.extent.height,
		                    static_cast<uint32_t>(barriers[0].oldLayout), static_cast<uint32_t>(barriers[0].newLayout),
		                    static_cast<unsigned long long>(static_cast<uint64_t>(barriers[0].srcAccessMask)),
		                    static_cast<unsigned long long>(static_cast<uint64_t>(barriers[0].dstAccessMask)),
		                    __builtin_return_address(0));
	}
#endif
	if (GpuProfiler::Enabled() && m_scheduler.Active() && m_scheduler.Current().IsRendering()) {
		// Name the transition (and who asked for it) that ends the pass.
		const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
		GpuProfiler::SetEndCaller(__builtin_return_address(0));
		const auto old_layout = static_cast<uint32_t>(barriers[0].oldLayout);
		const auto new_layout = static_cast<uint32_t>(barriers[0].newLayout);
		Common::DebugFlags::NoteOnce("CapyPS5-Transit",
		                             (caller << 20u) ^ (static_cast<uint64_t>(old_layout) << 10u) ^ new_layout ^
		                                 (track_addr << 1u),
		                             "pass ended by transit addr=0x%llx %ux%u layout %u -> %u access 0x%llx caller %p",
		                             static_cast<unsigned long long>(track_addr), backing.extent.width,
		                             backing.extent.height, old_layout, new_layout,
		                             static_cast<unsigned long long>(static_cast<uint64_t>(destination_access)),
		                             __builtin_return_address(0));
	}
	m_scheduler.EndRendering();
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
	dependency.pImageMemoryBarriers    = barriers.data();
	command_buffer.pipelineBarrier2(dependency);
}

void Image::Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
                   uint64_t size) {
	EXIT_IF(copies.empty() || buffer == nullptr || size == 0);
	if (static_cast<bool>(FullAspectMask(backing.format) & vk::ImageAspectFlagBits::eDepth)) {
		Common::DebugFlags::LogDepthWrite("Upload", track_addr);
	}
	m_scheduler.EndRendering();
	if (IsScaled()) {
		UploadScaled(copies, buffer, offset, size);
		return;
	}
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	buffer_barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = buffer;
	buffer_barrier.offset              = offset;
	buffer_barrier.size                = size;
	const auto image_barriers =
	    GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	                vk::PipelineStageFlagBits2::eCopy, {});
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	dependency.imageMemoryBarrierCount  = static_cast<uint32_t>(image_barriers.size());
	dependency.pImageMemoryBarriers     = image_barriers.data();
	auto command                        = m_scheduler.Current().Handle();
	command.pipelineBarrier2(dependency);
	command.copyBufferToImage(buffer, backing.image, vk::ImageLayout::eTransferDstOptimal,
	                          static_cast<uint32_t>(copies.size()), copies.data());
	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	buffer_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	dependency.imageMemoryBarrierCount = 0;
	dependency.pImageMemoryBarriers    = nullptr;
	command.pipelineBarrier2(dependency);
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

void Image::Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer,
                     uint64_t offset, uint64_t size) {
	EXIT_IF(copies.empty() || buffer == nullptr || size == 0);
	m_scheduler.EndRendering();
	if (IsScaled()) {
		DownloadScaled(copies, buffer, offset, size);
		return;
	}
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask        = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = buffer;
	buffer_barrier.offset              = offset;
	buffer_barrier.size                = size;
	const auto image_barriers =
	    GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	                vk::PipelineStageFlagBits2::eCopy, {});
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	dependency.imageMemoryBarrierCount  = static_cast<uint32_t>(image_barriers.size());
	dependency.pImageMemoryBarriers     = image_barriers.data();
	auto command                        = m_scheduler.Current().Handle();
	command.pipelineBarrier2(dependency);
	command.copyImageToBuffer(backing.image, vk::ImageLayout::eTransferSrcOptimal, buffer,
	                          static_cast<uint32_t>(copies.size()), copies.data());
	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	dependency.imageMemoryBarrierCount = 0;
	dependency.pImageMemoryBarriers    = nullptr;
	command.pipelineBarrier2(dependency);
}

std::pair<uint32_t, uint32_t> Image::SanitizeCopyLayers(const Image& source,
                                                        const Image& destination, uint32_t depth) {
	const auto source_type        = source.backing.image_type;
	const auto destination_type   = destination.backing.image_type;
	uint32_t   source_layers      = source.backing.layers;
	uint32_t   destination_layers = destination.backing.layers;
	if (source_type == vk::ImageType::e3D) {
		source_layers = 1;
	}
	if (destination_type == vk::ImageType::e3D) {
		destination_layers = 1;
	}
	if (source_type == destination_type) {
		source_layers = destination_layers = std::min(source_layers, destination_layers);
	} else if (source_type == vk::ImageType::e2D && destination_type == vk::ImageType::e3D) {
		source_layers = depth;
	} else if (source_type == vk::ImageType::e3D && destination_type == vk::ImageType::e2D) {
		destination_layers = depth;
	}
	return {source_layers, destination_layers};
}

void Image::CopyImage(Image& source) {
	EXIT_IF(source.backing.samples != backing.samples);
	if (source.scale != scale) {
		BlitImage(source);
		return;
	}
	if (static_cast<bool>(FullAspectMask(backing.format) & vk::ImageAspectFlagBits::eDepth)) {
		Common::DebugFlags::LogDepthWrite("CopyImage", track_addr);
	}
	m_scheduler.EndRendering();
	const uint32_t levels     = std::min(source.backing.mip_levels, backing.mip_levels);
	const uint32_t base_depth = source.backing.image_type == backing.image_type
	                                ? std::min(source.backing.extent.depth, backing.extent.depth)
	                            : backing.image_type == vk::ImageType::e3D
	                                ? backing.extent.depth
	                                : source.backing.extent.depth;
	const auto     source_aspect =
	    FullAspectMask(source.backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	const auto destination_aspect =
	    FullAspectMask(backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	std::vector<vk::ImageCopy> copies;
	copies.reserve(levels);
	for (uint32_t level = 0; level < levels; level++) {
		const auto width  = std::max(source.backing.extent.width >> level, 1u);
		const auto height = std::max(source.backing.extent.height >> level, 1u);
		const auto depth  = std::max(base_depth >> level, 1u);
		const auto [source_layers, destination_layers] = SanitizeCopyLayers(source, *this, depth);
		vk::ImageCopy copy {};
		copy.srcSubresource = {source_aspect, level, 0, 1};
		copy.dstSubresource = {destination_aspect, level, 0, 1};
		if (source.backing.image_type == backing.image_type) {
			if (source.backing.image_type == vk::ImageType::e3D) {
				copy.extent = {width, height, depth};
			} else {
				copy.srcSubresource.layerCount = std::min(source_layers, destination_layers);
				copy.dstSubresource.layerCount = copy.srcSubresource.layerCount;
				copy.extent                    = {width, height, 1};
			}
		} else if (source.backing.image_type == vk::ImageType::e2D) {
			copy.srcSubresource.layerCount = source_layers;
			copy.extent                    = {width, height, source_layers};
		} else {
			copy.dstSubresource.layerCount = destination_layers;
			copy.extent                    = {width, height, destination_layers};
		}
		copies.push_back(copy);
	}
	if (copies.empty()) {
		return;
	}
	auto command = m_scheduler.Current().Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, backing.image,
	                  vk::ImageLayout::eTransferDstOptimal, static_cast<uint32_t>(copies.size()),
	                  copies.data());
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

void Image::Resolve(Image& source, const ImageSubresourceRange& source_range,
                    const ImageSubresourceRange& destination_range) {
	EXIT_IF(backing.samples != 1 || source.backing.image_type != vk::ImageType::e2D ||
	        backing.image_type != vk::ImageType::e2D || source_range.level_count != 1 ||
	        destination_range.level_count != 1 ||
	        source_range.base_level >= source.backing.mip_levels ||
	        destination_range.base_level >= backing.mip_levels ||
	        source_range.base_layer >= source.backing.layers ||
	        destination_range.base_layer >= backing.layers);
	const auto layers       = std::min({source_range.layer_count, destination_range.layer_count,
	                                    source.backing.layers - source_range.base_layer,
	                                    backing.layers - destination_range.base_layer});
	const auto source_width = std::max(source.backing.extent.width >> source_range.base_level, 1u);
	const auto source_height =
	    std::max(source.backing.extent.height >> source_range.base_level, 1u);
	const auto destination_width =
	    std::max(backing.extent.width >> destination_range.base_level, 1u);
	const auto destination_height =
	    std::max(backing.extent.height >> destination_range.base_level, 1u);
	const bool copy = source.backing.samples == 1;
	EXIT_IF(source.scale != scale);
	const auto host_extent = ScaledExtent(info.extent, scale);
	EXIT_IF(layers == 0 || host_extent.width > source_width || host_extent.height > source_height ||
	        host_extent.width > destination_width || host_extent.height > destination_height ||
	        (copy ? !ImageViewOps::FormatsCompatible(source.backing.format, backing.format)
	              : source.backing.format != backing.format));
	auto resolved_source_range             = source_range;
	auto resolved_destination_range        = destination_range;
	resolved_source_range.layer_count      = layers;
	resolved_destination_range.layer_count = layers;
	const vk::Extent3D resolve_extent {host_extent.width, host_extent.height, 1};

	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	               resolved_source_range, command);
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	        resolved_destination_range, command);
	if (copy) {
		vk::ImageCopy region {};
		region.srcSubresource = {vk::ImageAspectFlagBits::eColor, resolved_source_range.base_level,
		                         resolved_source_range.base_layer, layers};
		region.dstSubresource = {vk::ImageAspectFlagBits::eColor,
		                         resolved_destination_range.base_level,
		                         resolved_destination_range.base_layer, layers};
		region.extent         = resolve_extent;
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, backing.image,
		                  vk::ImageLayout::eTransferDstOptimal, region);
	} else {
		vk::ImageResolve region {};
		region.srcSubresource = {vk::ImageAspectFlagBits::eColor, resolved_source_range.base_level,
		                         resolved_source_range.base_layer, layers};
		region.dstSubresource = {vk::ImageAspectFlagBits::eColor,
		                         resolved_destination_range.base_level,
		                         resolved_destination_range.base_layer, layers};
		region.extent         = resolve_extent;
		command.resolveImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                     backing.image, vk::ImageLayout::eTransferDstOptimal, region);
	}
}

uint32_t Image::CopyRows(uint64_t row_size, uint32_t rows, uint64_t capacity) noexcept {
	if (row_size == 0 || rows == 0 || row_size > capacity) {
		return 0;
	}
	return static_cast<uint32_t>(std::min<uint64_t>(rows, capacity / row_size));
}

void Image::CopyImageWithBuffer(Image& source, Buffer& buffer, TileManager& tiler) {
	EXIT_IF(buffer.Handle() == nullptr || source.backing.samples != 1 || backing.samples != 1);
	if (static_cast<bool>(FullAspectMask(backing.format) & vk::ImageAspectFlagBits::eDepth)) {
		Common::DebugFlags::LogDepthWrite("CopyImageWithBuffer", track_addr);
	}
	m_scheduler.EndRendering();
	const uint32_t levels = std::min(source.backing.mip_levels, backing.mip_levels);
	const auto     source_aspect =
	    FullAspectMask(source.backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	const auto destination_aspect =
	    FullAspectMask(backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	const auto     source_bytes      = DepthAspectTransferBytes(source.backing.format) != 0
	                                       ? DepthAspectTransferBytes(source.backing.format)
	                                       : source.info.bytes_per_block;
	const auto     destination_bytes = DepthAspectTransferBytes(backing.format) != 0
	                                       ? DepthAspectTransferBytes(backing.format)
	                                       : info.bytes_per_block;
	const uint32_t source_block      = source.info.IsBlock() ? 4u : 1u;
	const uint32_t destination_block = info.IsBlock() ? 4u : 1u;
	EXIT_IF(levels == 0 || source_bytes == 0 || source_bytes != destination_bytes ||
	        source_block != destination_block);
	const auto source_transform = source.info.GetColorTransform();
	const auto target_transform = info.GetColorTransform();

	vk::BufferMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eTransferRead;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer.Handle();
	barrier.offset              = 0;
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &barrier;
	auto command                        = m_scheduler.Current().Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	for (uint32_t level = 0; level < levels; level++) {
		const auto width             = std::max(source.backing.extent.width >> level, 1u);
		const auto height            = std::max(source.backing.extent.height >> level, 1u);
		const auto source_depth      = source.backing.image_type == vk::ImageType::e3D
		                                   ? std::max(source.backing.extent.depth >> level, 1u)
		                                   : source.backing.layers;
		const auto destination_depth = backing.image_type == vk::ImageType::e3D
		                                   ? std::max(backing.extent.depth >> level, 1u)
		                                   : backing.layers;
		const auto slices            = std::min(source_depth, destination_depth);
		const auto block_rows        = (height + source_block - 1) / source_block;
		const auto row_size =
		    static_cast<uint64_t>((width + source_block - 1) / source_block) * source_bytes;
		const auto rows_per_copy = CopyRows(row_size, block_rows, buffer.Size());
		EXIT_IF(slices == 0 || rows_per_copy == 0);
		for (uint32_t slice = 0; slice < slices; slice++) {
			for (uint32_t block_row = 0; block_row < block_rows; block_row += rows_per_copy) {
				const auto          copy_rows   = std::min(rows_per_copy, block_rows - block_row);
				const auto          y           = block_row * source_block;
				const auto          copy_height = std::min(copy_rows * source_block, height - y);
				const auto          copy_size   = row_size * copy_rows;
				vk::BufferImageCopy source_copy {};
				source_copy.imageSubresource = {
				    source_aspect, level,
				    source.backing.image_type == vk::ImageType::e3D ? 0u : slice, 1};
				source_copy.imageOffset           = {0, static_cast<int32_t>(y),
				                                     source.backing.image_type == vk::ImageType::e3D
				                                         ? static_cast<int32_t>(slice)
				                                         : 0};
				source_copy.imageExtent           = {width, copy_height, 1};
				auto destination_copy             = source_copy;
				destination_copy.imageSubresource = {
				    destination_aspect, level,
				    backing.image_type == vk::ImageType::e3D ? 0u : slice, 1};
				destination_copy.imageOffset.z =
				    backing.image_type == vk::ImageType::e3D ? static_cast<int32_t>(slice) : 0;
				barrier.size          = copy_size;
				barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
				barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
				command.pipelineBarrier2(dependency);
				command.copyImageToBuffer(source.backing.image,
				                          vk::ImageLayout::eTransferSrcOptimal, buffer.Handle(),
				                          source_copy);
				if (source_transform != target_transform) {
					const TileManager::Result bytes {buffer.Handle(), 0, copy_size};
					if (source_transform != ColorTransform::None) {
						tiler.TransformColor(bytes, bytes, source_transform, false);
					}
					if (target_transform != ColorTransform::None) {
						tiler.TransformColor(bytes, bytes, target_transform, true);
					}
				} else {
					barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
					barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
					command.pipelineBarrier2(dependency);
				}
				command.copyBufferToImage(buffer.Handle(), backing.image,
				                          vk::ImageLayout::eTransferDstOptimal, destination_copy);
			}
		}
	}
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

void Image::CopyMip(Image& source, uint32_t mip, uint32_t layer) {
	EXIT_IF(source.backing.samples != backing.samples || mip >= backing.mip_levels ||
	        layer >= backing.layers);
	if (static_cast<bool>(FullAspectMask(backing.format) & vk::ImageAspectFlagBits::eDepth)) {
		Common::DebugFlags::LogDepthWrite("CopyMip", track_addr);
	}
	m_scheduler.EndRendering();
	const auto width  = std::max(backing.extent.width >> mip, 1u);
	const auto height = std::max(backing.extent.height >> mip, 1u);
	const auto depth  = std::max(backing.extent.depth >> mip, 1u);
	if (source.scale != scale) {
		BlitMip(source, mip, layer);
		return;
	}
	EXIT_IF(width != source.backing.extent.width || height != source.backing.extent.height);
	const auto [source_layers, destination_layers] = SanitizeCopyLayers(source, *this, depth);
	const auto aspects                             = FullAspectMask(source.backing.format);
	EXIT_IF(aspects != FullAspectMask(backing.format));
	std::array<vk::ImageCopy, 2> copies {};
	uint32_t                     copy_count = 0;
	for (const auto aspect: {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
	                         vk::ImageAspectFlagBits::eStencil}) {
		if (!static_cast<bool>(aspects & aspect)) {
			continue;
		}
		auto& copy          = copies[copy_count++];
		copy.srcSubresource = {aspect, 0, 0, source_layers};
		copy.dstSubresource = {aspect, mip, layer, destination_layers};
		copy.extent         = {width, height, depth};
	}
	auto command = m_scheduler.Current().Handle();
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, backing.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy_count, copies.data());
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

void Image::UploadScaled(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer,
                         uint64_t offset, uint64_t size) {
	const auto aspect  = FullAspectMask(backing.format);
	auto       command = m_scheduler.Current().Handle();
	if (aspect != vk::ImageAspectFlagBits::eColor) {
		// Depth cannot be resampled by a blit on every host; a scaled depth target starts
		// from undefined contents, which games clear before use.
		Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x5344555055ull,
		                             "scaled depth image: guest contents not uploaded");
		Transit(vk::ImageLayout::eGeneral,
		        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
		return;
	}
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	buffer_barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = buffer;
	buffer_barrier.offset              = offset;
	buffer_barrier.size                = size;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	command.pipelineBarrier2(dependency);
	auto full = CreateTransferImage(m_graphics, backing, info.extent);
	TransferImageBarrier(command, full->image, aspect, vk::ImageLayout::eUndefined,
	                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits2::eTransferWrite);
	command.copyBufferToImage(buffer, full->image, vk::ImageLayout::eTransferDstOptimal,
	                          static_cast<uint32_t>(copies.size()), copies.data());
	TransferImageBarrier(command, full->image, aspect, vk::ImageLayout::eTransferDstOptimal,
	                     vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
	                     vk::AccessFlagBits2::eTransferRead);
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	BlitLevels(m_graphics, command, full->image, info.extent, backing.image, backing.extent,
	           backing.format, aspect, backing.mip_levels, backing.layers);
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
	m_scheduler.DeferOperation([graphics = &m_graphics, full = std::move(full)]() mutable {
		graphics->DeleteImage(*full);
	});
}

void Image::DownloadScaled(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer,
                           uint64_t offset, uint64_t size) {
	const auto aspect  = FullAspectMask(backing.format);
	auto       command = m_scheduler.Current().Handle();
	if (aspect != vk::ImageAspectFlagBits::eColor) {
		Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x5344444f574eull,
		                             "scaled depth image: contents not written back to the guest");
		return;
	}
	auto full = CreateTransferImage(m_graphics, backing, info.extent);
	TransferImageBarrier(command, full->image, aspect, vk::ImageLayout::eUndefined,
	                     vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits2::eTransferWrite);
	Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, command);
	BlitLevels(m_graphics, command, backing.image, backing.extent, full->image, info.extent,
	           backing.format, aspect, backing.mip_levels, backing.layers);
	TransferImageBarrier(command, full->image, aspect, vk::ImageLayout::eTransferDstOptimal,
	                     vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
	                     vk::AccessFlagBits2::eTransferRead);
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask        = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = buffer;
	buffer_barrier.offset              = offset;
	buffer_barrier.size                = size;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	command.pipelineBarrier2(dependency);
	command.copyImageToBuffer(full->image, vk::ImageLayout::eTransferSrcOptimal, buffer,
	                          static_cast<uint32_t>(copies.size()), copies.data());
	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	command.pipelineBarrier2(dependency);
	m_scheduler.DeferOperation([graphics = &m_graphics, full = std::move(full)]() mutable {
		graphics->DeleteImage(*full);
	});
}

void Image::BlitImage(Image& source) {
	const auto aspect = FullAspectMask(backing.format);
	if (aspect != vk::ImageAspectFlagBits::eColor ||
	    FullAspectMask(source.backing.format) != vk::ImageAspectFlagBits::eColor ||
	    source.backing.image_type != backing.image_type) {
		Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x5342434f5059ull,
		                             "copy between differently scaled depth images skipped");
		return;
	}
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	BlitLevels(m_graphics, command, source.backing.image, source.backing.extent, backing.image,
	           backing.extent, backing.format, aspect,
	           std::min(source.backing.mip_levels, backing.mip_levels),
	           std::min(source.backing.layers, backing.layers));
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

void Image::BlitMip(Image& source, uint32_t mip, uint32_t layer) {
	const auto aspect = FullAspectMask(backing.format);
	if (aspect != vk::ImageAspectFlagBits::eColor ||
	    FullAspectMask(source.backing.format) != vk::ImageAspectFlagBits::eColor) {
		Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x53424d4950ull,
		                             "mip copy between differently scaled depth images skipped");
		return;
	}
	auto command = m_scheduler.Current().Handle();
	Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command);
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	const auto  layers = std::min(source.backing.layers, backing.layers - layer);
	vk::ImageBlit region {};
	region.srcSubresource = {aspect, 0, 0, layers};
	region.dstSubresource = {aspect, mip, layer, layers};
	region.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(source.backing.extent.width),
	                                      static_cast<int32_t>(source.backing.extent.height), 1};
	region.dstOffsets[1]  = vk::Offset3D {
	     static_cast<int32_t>(std::max(backing.extent.width >> mip, 1u)),
	     static_cast<int32_t>(std::max(backing.extent.height >> mip, 1u)), 1};
	command.blitImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, backing.image,
	                  vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
	Transit(vk::ImageLayout::eGeneral,
	        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {}, command);
}

namespace ImageOps {

void Validate(const ImageInfo& info) {
	ValidateOptionalRange(info.data, "data");
	ValidateOptionalRange(info.stencil, "stencil");

	if (info.pixel_format == vk::Format::eUndefined) {
		const bool metadata_empty =
		    info.metadata.range.Empty() && info.metadata.kind == ImageMetadataKind::None &&
		    info.metadata.control == 0 &&
		    info.metadata.compression == VideoOutCompression::Uncompressed &&
		    !info.metadata.stencil_compressed;
		if (info.data.Empty() || info.HasStencil() || !metadata_empty || info.extent.width == 0 ||
		    info.extent.height == 0 || info.extent.depth == 0 || info.resources.levels != 1 ||
		    info.resources.layers != 1 || info.samples != 1 || info.pitch != 0 ||
		    info.bytes_per_block != 0) {
			EXIT("invalid stencil association image\n");
		}
		return;
	}

	if (info.extent.width == 0 || info.extent.height == 0 || info.extent.depth == 0 ||
	    info.resources.levels == 0 || info.resources.levels > info.mip_layout.size() ||
	    info.resources.layers == 0 || info.samples == 0 ||
	    vulkan_sample_count(info.samples) == vk::SampleCountFlagBits {} ||
	    info.bytes_per_block == 0 || (info.data.address != 0 && info.pitch == 0)) {
		EXIT("invalid image geometry or format\n");
	}

	switch (info.type) {
		case Prospero::ImageType::kColor1D:
			if (info.extent.height != 1 || info.extent.depth != 1) {
				EXIT("invalid 1D image shape\n");
			}
			break;
		case Prospero::ImageType::kColor3D:
			if (info.resources.layers != 1) {
				EXIT("3D images cannot have array layers\n");
			}
			break;
		case Prospero::ImageType::kColor2D:
			if (info.extent.depth != 1) {
				EXIT("invalid 2D image shape\n");
			}
			break;
		default: EXIT("non-base image type: %u\n", static_cast<uint32_t>(info.type));
	}
	if (info.samples > 1 && info.resources.levels != 1) {
		EXIT("multisampled images cannot have mip levels\n");
	}

	if (info.metadata.stencil_compressed && !info.HasStencil()) {
		EXIT("compressed stencil metadata requires a stencil plane\n");
	}
	switch (info.metadata.kind) {
		case ImageMetadataKind::None:
			if (!info.metadata.range.Empty() || info.metadata.control != 0 ||
			    info.metadata.compression != VideoOutCompression::Uncompressed ||
			    info.metadata.stencil_compressed) {
				EXIT("metadata-free image has metadata state\n");
			}
			break;
		case ImageMetadataKind::Htile:
			if (!info.metadata.range.Valid() ||
			    info.metadata.compression != VideoOutCompression::Uncompressed) {
				EXIT("invalid HTILE metadata\n");
			}
			break;
		case ImageMetadataKind::Dcc:
		case ImageMetadataKind::Cmask:
			if (!GuestRange {info.metadata.range.address,
			                 std::max<uint64_t>(info.metadata.range.size, 1)}.Valid() ||
			    info.metadata.compression == VideoOutCompression::Unsupported) {
				EXIT("invalid color metadata\n");
			}
			break;
	}
}

Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element) {
	switch (bytes_per_element) {
		case 1: return Prospero::BufferFormat::k8UNorm;
		case 2: return Prospero::BufferFormat::k16UNorm;
		case 4: return Prospero::BufferFormat::k32Float;
		case 8: return Prospero::BufferFormat::k16_16_16_16Float;
		case 16: return Prospero::BufferFormat::k32_32_32_32Float;
		default: EXIT("unsupported render-target element size: %u\n", bytes_per_element);
	}
}

} // namespace ImageOps

Image::Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& image_info,
             float image_scale)
    : info(image_info), scale(image_info.type == Prospero::ImageType::kColor2D ? image_scale : 1.0f),
      stencil_subresources {0, image_info.resources.levels, 0, image_info.resources.layers},
      m_graphics(graphics), m_scheduler(scheduler) {
	KYTY_PROFILER_FUNCTION();
	ImageOps::Validate(info);
	m_cpu_dirty =
	    !info.data.Empty() && info.metadata.compression == VideoOutCompression::Uncompressed;
	if (info.pixel_format == vk::Format::eUndefined) {
		return;
	}

	vk::ImageCreateInfo create {};
	create.usage         = ImageUsageFlags(graphics, info);
	create.flags         = ImageCreateFlags(graphics, info, create.usage);
	create.imageType     = HostImageType(info.type);
	create.extent        = ScaledExtent(info.extent, scale);
	create.mipLevels     = info.resources.levels;
	create.arrayLayers   = info.IsVolume() ? 1u : info.resources.layers;
	create.format        = info.pixel_format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.samples       = vulkan_sample_count(info.samples);
	// Measurement probe: `setprop debug.capy.ubwc_probe 1` declares only the image's own format
	// for render targets, which lets drivers keep framebuffer compression (UBWC) despite
	// eMutableFormat. Views in other formats then read wrong data; for FPS comparisons only.
	static Common::DebugFlags::CachedValue ubwc_probe("debug.capy.ubwc_probe", 0);
	vk::ImageFormatListCreateInfo own_format {};
	own_format.viewFormatCount = 1;
	own_format.pViewFormats    = &create.format;
	if (ubwc_probe.Get() != 0 && !info.IsBlock() &&
	    (create.usage & (vk::ImageUsageFlagBits::eColorAttachment |
	                     vk::ImageUsageFlagBits::eDepthStencilAttachment))) {
		create.pNext = &own_format;
	}

	vk::ImageFormatProperties properties {};
	if (graphics.GetImageFormatProperties(create.format, create.imageType, create.tiling,
	                                      create.usage, create.flags,
	                                      &properties) != vk::Result::eSuccess ||
	    !static_cast<bool>(properties.sampleCounts & create.samples)) {
		EXIT("image format does not support required usage: format=%d type=%d usage=0x%x "
		     "flags=0x%x samples=%u\n",
		     static_cast<int>(create.format), static_cast<int>(create.imageType),
		     static_cast<vk::ImageUsageFlags::MaskType>(create.usage),
		     static_cast<vk::ImageCreateFlags::MaskType>(create.flags), info.samples);
	}

	if (!graphics.CreateImage(create, backing)) {
		EXIT("failed to create image: extent=%ux%ux%u format=%d layers=%u levels=%u\n",
		     create.extent.width, create.extent.height, create.extent.depth,
		     static_cast<int>(create.format), create.arrayLayers, create.mipLevels);
	}
	SetVulkanObjectNameF(
	    graphics.device, backing.image,
	    "Kyty.Image[guest=0x{:016x} size=0x{:x} extent={}x{}x{} format={} mips={} layers={} samples={}]",
	    info.data.address, info.data.size, info.extent.width, info.extent.height, info.extent.depth,
	    static_cast<uint32_t>(info.pixel_format), info.resources.levels, info.resources.layers,
	    info.samples);
}

uint64_t Image::HashGuestEdges() const {
	std::array<uint8_t, TRACKER_PAGE_SIZE * 2> bytes {};
	const auto                                 range = info.data;
	const uint64_t head_end =
	    std::min(range.End(), Common::AlignUp(range.address, TRACKER_PAGE_SIZE));
	const uint64_t tail_begin =
	    std::max(range.address, Common::AlignDown(range.End(), TRACKER_PAGE_SIZE));
	const uint64_t head_size    = head_end - range.address;
	const uint64_t tail_address = tail_begin < head_end ? head_end : tail_begin;
	const uint64_t tail_size    = range.End() - tail_address;
	if ((head_size != 0 &&
	     !LibKernel::Memory::TryReadBacking(range.address, bytes.data(), head_size)) ||
	    (tail_size != 0 &&
	     !LibKernel::Memory::TryReadBacking(tail_address, bytes.data() + head_size, tail_size))) {
		EXIT("Image: failed to hash guest backing\n");
	}
	return XXH3_64bits(bytes.data(), static_cast<size_t>(head_size + tail_size));
}

Image::~Image() {
	KYTY_PROFILER_FUNCTION();
	for (const auto& cached: views) {
		if (cached.view != nullptr) {
			m_graphics.device.destroyImageView(cached.view, nullptr);
		}
	}
	if (backing.image != nullptr) {
		m_graphics.DeleteImage(backing);
	}
}

} // namespace Libs::Graphics
