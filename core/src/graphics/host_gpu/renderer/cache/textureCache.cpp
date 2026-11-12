// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/telemetry.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "kernel/memory.h"

#include <xxhash.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// `setprop debug.capy.clear_r11 0`: skip register clears of B10G11R11 targets (bisection).
		static Common::DebugFlags::CachedValue clear_r11("debug.capy.clear_r11", 1);
		if (clear_r11.Get() == 0 && format == vk::Format::eB10G11R11UfloatPack32) {
			return false;
		}
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodePackedColorClear64(format, metadata.clear_word, metadata.clear_word_high, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	if (m_graphics.bc_textures_decoded) {
		m_bc_decoder = std::make_unique<BcDecoder>(graphics, scheduler);
	}
	if (m_graphics.CanReportMemoryUsage()) {
		constexpr int64_t GiB = 1024ll * 1024 * 1024;
		const auto        budget =
		    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
		const auto threshold = std::min<int64_t>(budget, 8 * GiB);
		m_pressure_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
		m_critical_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
		// Upstream sizes this for discrete GPUs: with a budget of 8 GiB or less it is 0 and the
		// collector evicts every idle image each frame. Start below the pressure level instead. On unified-memory phones that kept
		// re-uploading alternating 4096x4096 backgrounds and flickered Dead Cells' darkness layer.
		m_trigger_gc_memory = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, budget * 3 / 10));
	} else {
		// Without a memory budget report (Adreno) the thresholds stayed 0 and the collector
		// evicted every idle image each frame, re-uploading Dead Cells' alternating 4096x4096
		// backgrounds and flickering its darkness layer. Use fixed unified-memory limits.
		constexpr uint64_t MiB = 1024ull * 1024;
		m_trigger_gc_memory  = 1536 * MiB;
		m_pressure_gc_memory = 2560 * MiB;
		m_critical_gc_memory = 3584 * MiB;
	}
}

TextureCache::~TextureCache() {
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (cached.GetColorTransform() != requested.GetColorTransform()) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

float TextureCache::ResolutionScale() {
	// `setprop debug.capy.res_scale N` (per mille, read once): render large targets at N/1000
	// of their guest size. 1000 or 0 keeps native resolution.
	static const float scale = [] {
		Common::DebugFlags::CachedValue value("debug.capy.res_scale", 1000);
		const auto permille = value.Get();
		return permille <= 0 || permille >= 1000 ? 1.0f : static_cast<float>(permille) / 1000.0f;
	}();
	return scale;
}

float TextureCache::ShadowScale() {
	// `debug.capy.shadow_scale N` (per mille, read once): render shadow maps at N/1000 of their
	// guest size. 1000 or 0 leaves them to debug.capy.res_scale.
	static const float scale = [] {
		Common::DebugFlags::CachedValue value("debug.capy.shadow_scale", 1000);
		const auto permille = value.Get();
		return permille <= 0 || permille >= 1000 ? 1.0f : static_cast<float>(permille) / 1000.0f;
	}();
	return scale;
}

float TextureCache::ChooseScale(const ImageDesc& desc) {
	const auto& info = desc.info;
	// Shadow maps: square single-sample depth targets of 1024 texels or more. Screen depth
	// buffers follow the display aspect and are never square.
	if (ShadowScale() != 1.0f && desc.type == BindingType::DepthTarget &&
	    info.type == Prospero::ImageType::kColor2D && info.samples == 1 &&
	    info.resources.levels == 1 && info.extent.width == info.extent.height &&
	    info.extent.width >= 1024) {
		return ShadowScale();
	}
	const float scale = ResolutionScale();
	if (scale == 1.0f) {
		return 1.0f;
	}
	// Display buffers are scaled too: Greak flips one of its two buffers before drawing into it,
	// and that buffer, created unscaled, was then drawn with the scaled depth target bound, so
	// every other frame showed only the top-left quarter of the scene.
	if ((desc.type != BindingType::RenderTarget && desc.type != BindingType::DepthTarget &&
	     desc.type != BindingType::VideoOut) ||
	    info.type != Prospero::ImageType::kColor2D || info.samples != 1 ||
	    info.resources.levels != 1 || info.IsBlock() || info.extent.width < 1280 ||
	    info.extent.height < 720) {
		return 1.0f;
	}
	return scale;
}

ImageId TextureCache::InsertImage(const ImageInfo& info, float scale) {
	const auto id = m_slot_images.insert(m_graphics, m_scheduler, info, scale);
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (!image->depth_id) {
		std::vector<ImageId> associations;
		m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
			if (associated.depth_id == id) {
				associations.push_back(candidate);
			}
		});
		for (const auto association: associations) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile && metadata->second.owner == id) {
			// A later binding may have reused this address for another metadata type, and only
			// the last image to acquire an HTile range releases its clear state.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	Common::DebugFlags::FrameTrace("  tc free id=%u addr=0x%llx %ux%u", static_cast<uint32_t>(id.index),
	                               static_cast<unsigned long long>(m_slot_images[id].info.data.address),
	                               m_slot_images[id].info.extent.width, m_slot_images[id].info.extent.height);
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled() && !image.info.data.Empty()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

bool TextureCache::FindTargetInfoAt(uint64_t address, uint32_t width, uint32_t height, ImageInfo* info) {
	std::scoped_lock lock(m_lock);
	for (const auto id: FindImagesInRegion(address, 1, false)) {
		const auto* image = m_slot_images.try_get(id);
		if (image != nullptr && image->info.data.address == address && image->info.extent.width == width &&
		    image->info.extent.height == height && image->info.IsTiled() &&
		    (image->binding.is_target || image->IsGpuModified())) {
			*info = image->info;
			return true;
		}
	}
	return false;
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	const auto format = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);
	return id;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	// Converts guest D16 between a four-byte host depth aspect and two-byte color or D16 depth.
	const bool source_wide = DepthAspectTransferBytes(source.backing.format) == sizeof(uint32_t);
	if (source_wide == (DepthAspectTransferBytes(destination.backing.format) == sizeof(uint32_t))) {
		return false;
	}
	auto& depth  = source_wide ? source : destination;
	auto& narrow = source_wide ? destination : source;
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    narrow.info.bytes_per_block != sizeof(uint16_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * sizeof(uint32_t);
	const uint64_t narrow_slice =
	    static_cast<uint64_t>(narrow.info.pitch) * narrow.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || narrow_slice > UINT64_MAX / layers);
	const std::array depth_copies {
	    vk::BufferImageCopy {0,
	                         depth.info.pitch,
	                         depth.info.extent.height,
	                         {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                         {},
	                         depth.info.extent}};
	const std::array narrow_copies {
	    vk::BufferImageCopy {0,
	                         narrow.info.pitch,
	                         narrow.info.extent.height,
	                         {narrow.info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
	                                                : vk::ImageAspectFlagBits::eColor,
	                          0, 0, layers},
	                         {},
	                         narrow.info.extent}};

	auto depth_buffer  = m_tiler.GetScratchBuffer(depth_slice * layers);
	auto narrow_buffer = m_tiler.GetScratchBuffer(narrow_slice * layers, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(narrow.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * sizeof(uint32_t),
	    .source_slice_stride = narrow_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_wide) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, narrow_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(narrow_copies, narrow_buffer.buffer, narrow_buffer.offset,
		                   narrow_buffer.size);
	} else {
		source.Download(narrow_copies, narrow_buffer.buffer, narrow_buffer.offset,
		                narrow_buffer.size);
		m_tiler.ConvertD16(narrow_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	Common::DebugFlags::FrameTrace("  tc %s id=%u addr=0x%llx", "copy-to", static_cast<uint32_t>(destination_id.index),
	                               static_cast<unsigned long long>(m_slot_images[destination_id].info.data.address));
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer, m_tiler);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264, PPSA04288
	// A partial view retains the entire matching array layout. HTile belongs to
	// the depth source, independently of the data slices copied into color storage.
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    (cached.info.metadata.kind == ImageMetadataKind::None ||
	     cached.info.metadata.kind == ImageMetadataKind::Htile);
	// Depth bytes sampled through a color format no depth view provides (16_UINT, 8_8_UNORM)
	// need a color copy of the whole single-sample depth layout.
	const bool raw_depth_texture =
	    cached.info.IsDepth() && !requested.IsDepth() && !cached.info.HasStencil() &&
	    !requested.HasMetadata() && cached.backing.samples == 1 && requested.samples == 1 &&
	    requested.pitch == cached.info.pitch &&
	    (requested.resources == cached.info.resources || retain_cached_layout) &&
	    !IsSupportedSampledDepthFormat(cached.info.pixel_format, requested.pixel_format);
	bool recreate = cached.info.resources < requested.resources ||
	                requested.IsVolume() != cached.info.IsVolume();
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_depth_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout          = cached.info.mip_layout;
		info.tiling              = cached.info.tiling;
		info.tiled_slice_stride  = cached.info.tiled_slice_stride;
		info.linear_slice_stride = cached.info.linear_slice_stride;
		info.first_tail_level    = cached.info.first_tail_level;
	}
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info, cached.scale);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	FreeImage(cached_id);
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	// A PS4 color target has no 3D mode: a volume is drawn slice by slice into layers and then
	// sampled as 3D (Unreal's color grading LUT). Vulkan cannot view one as the other, so the
	// image changes shape, keeping its contents, each time the use changes. Tiling and pitch differ
	// too: the T# of a thick surface is read as linear, the target is displayable.
	if (ShaderRecompiler::Decoder::GetGuestIsa() == ShaderRecompiler::Decoder::GuestIsa::Gfx7 &&
	    requested.data.address == cached.info.data.address && requested_block == cached_block &&
	    requested.IsVolume() != cached.info.IsVolume() &&
	    requested.extent.width == cached.info.extent.width &&
	    requested.extent.height == cached.info.extent.height &&
	    requested.TransferLayers() == cached.info.TransferLayers()) {
		return {ExpandImage(requested, cached_id)};
	}
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     !std::equal(requested.mip_layout.begin(), requested.mip_layout.end(),
		                 cached.info.mip_layout.begin(),
		                 [](const ImageMipInfo& left, const ImageMipInfo& right) {
			                 return left.offset == right.offset && left.size == right.size &&
			                        left.pitch == right.pitch && left.height == right.height &&
			                        left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
			                        left.surface_z == right.surface_z;
		                 }))) {
			if (safe_to_delete) {
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		if (requested.GetColorTransform() != cached.info.GetColorTransform()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		// A PS4 linear or 1D-tiled texture of another size at this address is new data the game
		// loaded where an old texture was: the old image is simply stale.
		const bool gfx7_loaded = (requested.gfx7_linear && cached.info.gfx7_linear) ||
		                         (requested.gfx7_micro_pitch != 0 && cached.info.gfx7_micro_pitch != 0);
		if (gfx7_loaded && !cached.IsGpuModified() &&
		    (requested.extent != cached.info.extent || requested.data.size != cached.info.data.size)) {
#if defined(__ANDROID__)
			static Common::DebugFlags::CachedValue profile("debug.capy.gpu_profile", 0);
			if (profile.Get() == 2) {
				__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Upload",
				                    "replace 0x%llx %ux%ux%u size 0x%llx fmt %u by %ux%ux%u size 0x%llx fmt %u",
				                    static_cast<unsigned long long>(requested.data.address), cached.info.extent.width,
				                    cached.info.extent.height, cached.info.extent.depth,
				                    static_cast<unsigned long long>(cached.info.data.size),
				                    static_cast<unsigned>(cached.info.guest_format), requested.extent.width,
				                    requested.extent.height, requested.extent.depth,
				                    static_cast<unsigned long long>(requested.data.size),
				                    static_cast<unsigned>(requested.guest_format));
			}
#endif
			FreeImage(cached_id);
			return {merged_id};
		}
		if (requested.type != cached.info.type) {
			return {merged_id};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info, m_slot_images[source_id].scale);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

bool TextureCache::CanDownload(const Image& image) const {
	const auto& info = image.info;
	// A PS4 target is laid out for gfx10 here and its data.size runs past the real surface:
	// writing it back would overwrite the game's next allocation.
	if (info.gfx7_guest_size != 0 && info.gfx7_guest_size < info.data.size) {
		return false;
	}
	// Decoded BCn texels cannot be encoded back into the guest's blocks.
	if (image.IsBcDecoded()) {
		return false;
	}
	if (info.samples != 1 || image.backing.samples != 1 || info.resources.layers == 0 ||
	    info.data.size % info.resources.layers != 0) {
		return false;
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		return IsSupportedDepthPlaneReadback(info) &&
		       Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
	}
	return info.metadata.compression == VideoOutCompression::Uncompressed &&
	       (!info.IsTiled() || !Prospero::IsFmaskTextureFormat(info.guest_format)) &&
	       info.LinearSize() != 0;
}

// Decodes a PS4 1D-thin texture on the CPU: 8x8 micro tiles in raster order, blocks inside a tile in
// the thin order (x0, y0, x1, y1, x2, y2), each level after the previous one.
void TextureCache::UploadGfx7MicroTiled(Image& image) {
	const auto&    info   = image.info;
	const uint32_t bytes  = info.bytes_per_block;
	const uint32_t tw     = info.tiling.texel_width;
	const uint32_t th     = info.tiling.texel_height;
	std::vector<uint8_t> guest(info.data.size);
	if (!LibKernel::Memory::ReadGuestMemory(info.data.address, guest.data(), guest.size())) {
		return;
	}
	uint64_t linear_size = 0;
	for (uint32_t level = 0; level < info.resources.levels; level++) {
		const auto active = info.MipExtent(level);
		linear_size += uint64_t {(active.width + tw - 1u) / tw} * ((active.height + th - 1u) / th) * bytes *
		               (info.gfx7_micro_thick ? active.depth : info.resources.layers);
	}
	auto staging = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                        vk::BufferUsageFlagBits::eTransferSrc, linear_size);
	auto*    out        = staging->Mapped().data();
	uint64_t out_offset = 0;
	uint64_t in_offset  = 0;
	std::vector<vk::BufferImageCopy> copies;
	for (uint32_t level = 0; level < info.resources.levels; level++) {
		const auto active   = info.MipExtent(level);
		const auto layout   = info.Gfx7MicroLevelLayout(level);
		const auto columns  = (active.width + tw - 1u) / tw;
		const auto rows     = (active.height + th - 1u) / th;
		const auto per_row  = layout.pitch / 8u;
		const auto tile_size = uint64_t {64u} * bytes;
		if (info.gfx7_micro_thick) {
			// 1D-thick volume: 8x8x4 micro tiles; the bit order inside one follows the texel size
			// (AMD addrlib ComputePixelIndexWithinMicroTile, as in shadPS4's tiling.comp).
			const auto thick_tile = tile_size * 4u;
			vk::BufferImageCopy copy {};
			copy.bufferOffset      = out_offset;
			copy.bufferRowLength   = columns * tw;
			copy.bufferImageHeight = rows * th;
			copy.imageSubresource  = {vk::ImageAspectFlagBits::eColor, level, 0, 1};
			copy.imageExtent       = vk::Extent3D {active.width, active.height, active.depth};
			copies.push_back(copy);
			for (uint32_t z = 0; z < active.depth; z++) {
				for (uint32_t y = 0; y < rows; y++) {
					for (uint32_t x = 0; x < columns; x++) {
						const auto bit = [](uint32_t value, uint32_t index) { return (value >> index) & 1u; };
						const uint32_t x0 = bit(x, 0), x1 = bit(x, 1), y0 = bit(y, 0), y1 = bit(y, 1);
						const uint32_t z0 = bit(z, 0), z1 = bit(z, 1);
						uint32_t low = 0;
						if (bytes <= 2u) {
							low = x0 | (y0 << 1u) | (x1 << 2u) | (y1 << 3u) | (z0 << 4u) | (z1 << 5u);
						} else if (bytes == 4u) {
							low = x0 | (y0 << 1u) | (x1 << 2u) | (z0 << 3u) | (y1 << 4u) | (z1 << 5u);
						} else {
							low = x0 | (y0 << 1u) | (z0 << 2u) | (x1 << 3u) | (y1 << 4u) | (z1 << 5u);
						}
						const uint32_t element = low | (bit(x, 2) << 6u) | (bit(y, 2) << 7u);
						const uint64_t source  = in_offset + uint64_t {z / 4u} * layout.size +
						                        ((y / 8u) * uint64_t {per_row} + x / 8u) * thick_tile +
						                        uint64_t {element} * bytes;
						if (source + bytes <= guest.size()) {
							std::memcpy(out + out_offset, guest.data() + source, bytes);
						}
						out_offset += bytes;
					}
				}
			}
			in_offset += layout.size * layout.groups;
			continue;
		}
		for (uint32_t layer = 0; layer < info.resources.layers; layer++) {
			const uint64_t slice = in_offset + layer * layout.size;
			vk::BufferImageCopy copy {};
			copy.bufferOffset      = out_offset;
			copy.bufferRowLength   = columns * tw;
			copy.bufferImageHeight = rows * th;
			copy.imageSubresource  = {vk::ImageAspectFlagBits::eColor, level, layer, 1};
			copy.imageExtent       = vk::Extent3D {active.width, active.height, 1};
			copies.push_back(copy);
			if (info.gfx7_linear) {
				for (uint32_t y = 0; y < rows; y++) {
					const uint64_t source = slice + uint64_t {y} * layout.pitch * bytes;
					if (source + uint64_t {columns} * bytes <= guest.size()) {
						std::memcpy(out + out_offset, guest.data() + source, uint64_t {columns} * bytes);
					}
					out_offset += uint64_t {columns} * bytes;
				}
				continue;
			}
			const auto detile = [&]<uint32_t Bytes>() {
				// Within a micro-tile row the texels come in pairs: x = 0,1 | 2,3 | 4,5 | 6,7 sit
				// at elements 0,1 | 4,5 | 16,17 | 20,21 (plus the row's y bits).
				constexpr std::array<uint32_t, 4> pair_element {0u, 4u, 16u, 20u};
				for (uint32_t y = 0; y < rows; y++) {
					const uint32_t row_element = ((y & 1u) << 1u) | ((y & 2u) << 2u) | ((y & 4u) << 3u);
					const uint64_t row_tiles   = slice + (y / 8u) * uint64_t {per_row} * tile_size;
					for (uint32_t x = 0; x < columns; x += 2u) {
						const uint32_t element = pair_element[(x / 2u) & 3u] | row_element;
						const uint64_t source  = row_tiles + (x / 8u) * tile_size + uint64_t {element} * Bytes;
						const uint32_t count   = std::min(2u, columns - x);
						if (source + uint64_t {count} * Bytes <= guest.size()) {
							std::memcpy(out + out_offset, guest.data() + source, Bytes);
							if (count == 2u) {
								std::memcpy(out + out_offset + Bytes, guest.data() + source + Bytes, Bytes);
							}
						}
						out_offset += uint64_t {count} * Bytes;
					}
				}
			};
			switch (bytes) {
				case 1u: detile.template operator()<1u>(); break;
				case 2u: detile.template operator()<2u>(); break;
				case 4u: detile.template operator()<4u>(); break;
				case 8u: detile.template operator()<8u>(); break;
				case 16u: detile.template operator()<16u>(); break;
				default:
					for (uint32_t y = 0; y < rows; y++) {
						for (uint32_t x = 0; x < columns; x++) {
							const uint32_t element = (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) |
							                         ((y & 2u) << 2u) | ((x & 4u) << 2u) | ((y & 4u) << 3u);
							const uint64_t source =
							    slice + ((y / 8u) * uint64_t {per_row} + x / 8u) * tile_size + uint64_t {element} * bytes;
							if (source + bytes <= guest.size()) {
								std::memcpy(out + out_offset, guest.data() + source, bytes);
							}
							out_offset += bytes;
						}
					}
					break;
			}
		}
		in_offset += layout.size * info.resources.layers;
	}
	staging->Flush(0, linear_size);
	image.Upload(copies, staging->Handle(), 0, linear_size);
	m_scheduler.DeferOperation([old = std::move(staging)]() mutable { old.reset(); });
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	if (image.info.gfx7_micro_pitch != 0u) {
		UploadGfx7MicroTiled(image);
		return;
	}
	Common::DebugFlags::FrameTrace("  tc %s id=%u addr=0x%llx", "upload", static_cast<uint32_t>(0),
	                               static_cast<unsigned long long>(image.info.data.address));
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto& info        = image.info;
	const auto  binding     = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	if (binding != BindingType::DepthTarget) {
		if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		if (binding == BindingType::VideoOut &&
		    info.metadata.compression != VideoOutCompression::Uncompressed) {
			EXIT("TextureCache: invalid video-out upload\n");
		}
		const auto linear = m_tiler.Detile(source.Handle(), source_offset, info);
		const auto copies = info.BufferCopies(linear.offset);
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
		return;
	}

	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto layers          = info.resources.layers;
	const auto full_slice_size = info.linear_slice_stride;
	auto       linear          = m_tiler.Detile(source.Handle(), source_offset, info);
	auto       copies          = info.BufferCopies(linear.offset);
	if (image.depth_id) {
		for (auto& copy: copies) {
			copy.imageSubresource.baseArrayLayer += image.stencil_subresources.base_layer;
		}
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		copies.resize(1);
		copies[0].bufferOffset      = linear.offset;
		copies[0].bufferRowLength   = info.pitch;
		copies[0].bufferImageHeight = info.extent.height;
		copies[0].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, 0, layers};
		copies[0].imageExtent       = info.extent;
	}
	destination.Upload(copies, linear.buffer, linear.offset, linear.size);
}

void TextureCache::InitializeImage(ImageId id) {
	Common::DebugFlags::FrameTrace("  tc %s id=%u addr=0x%llx", "initialize", static_cast<uint32_t>(id.index),
	                               static_cast<unsigned long long>(m_slot_images[id].info.data.address));
	auto& image = m_slot_images[id];
	if (image.info.data.Empty()) {
		return;
	}
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (Common::DebugFlags::VideoProbeEnabled() && image.info.extent.width == 1920 &&
	    image.info.extent.height == 1080 && image.info.guest_format == Prospero::BufferFormat::k8_8_8_8UNorm) {
		Common::DebugFlags::NoteOnce("CapyPS5-VideoProbe", image.info.data.address ^ 1u,
		    "initialize image=0x%llx upload=%d cpu_dirty=%d buffer_modified=%d",
		    static_cast<unsigned long long>(image.info.data.address), upload ? 1 : 0,
		    image.IsCpuDirty() ? 1 : 0, image.IsBufferModified() ? 1 : 0);
	}
	if (upload) {
		const auto [source, source_offset] =
		    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		if (source == nullptr) {
			EXIT("TextureCache: failed to obtain image upload source\n");
		}
		UploadImage(image, *source, source_offset);
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	const auto range = desc.info.metadata.range;
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
		if (range.size == 0 || desc.info.resources.levels != 1) {
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks (PS4 CMASK slices: 128 bytes).
	const uint64_t MetadataBlockSize =
	    ShaderRecompiler::Decoder::GetGuestIsa() == ShaderRecompiler::Decoder::GuestIsa::Gfx7 ? 0x80u : 0x1000u;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		if (ShaderRecompiler::Decoder::GetGuestIsa() == ShaderRecompiler::Decoder::GuestIsa::Gfx7) {
			// PS4 CMASK layouts follow gfx7 rules; one this check cannot slice keeps its contents.
			Common::DebugFlags::NoteOnce("CapyPS5-Meta", range.address ^ 0x434d41534bull,
			                             "PS4 CMASK 0x%llx size=%llu layers=%u: no fast clear",
			                             static_cast<unsigned long long>(range.address),
			                             static_cast<unsigned long long>(range.size), layers);
			return;
		}
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	if (m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		m_buffer_cache.ReadMemory(range.address, range.size, false);
	}
	const auto slice_size = range.size / layers;
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			// Metadata over pages with no memory (a partially resident resource): no fast clear
			// can be recorded there, so the image contents stand as they are.
			Common::DebugFlags::NoteOnce("CapyPS5-Meta", address >> 12u,
			                             "color metadata 0x%llx is not mapped: no fast clear",
			                             static_cast<unsigned long long>(address));
			continue;
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			EXIT("TextureCache: failed to read color metadata slice\n");
		}
		if (!std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; })) {
			continue;
		}
		{
			std::scoped_lock lock {m_lock};
			ClearImage(m_scheduler.Current(), id, view.format,
			           {vk::ImageAspectFlagBits::eColor, view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			m_buffer_cache.InvalidateMemory(address, slice_size);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	static Common::DebugFlags::CachedValue small_image_hash("debug.capy.small_image_hash", 1);
	Common::DebugFlags::FrameTrace("  tc %s id=%u addr=0x%llx", "refresh", static_cast<uint32_t>(id.index),
	                               static_cast<unsigned long long>(m_slot_images[id].info.data.address));
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		(void)image.ResolveMaybeCpuHash(hash);
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	// Write tracking works on pages: a small image sharing one with data the CPU rewrites every
	// frame came back dirty on every bind (God of War re-uploaded one 1x1 texture ~600 times a
	// second, each upload ending the render pass). Unchanged bytes keep the uploaded image.
	constexpr uint64_t SmallImageBytes = 64u * 1024u;
	const auto         small = image.info.data.size != 0 && image.info.data.size <= SmallImageBytes &&
	                   !image.IsGpuModified() && !image.IsBufferModified() &&
	                   image.IsDefinitelyCpuDirty() && small_image_hash.Get() != 0;
	uint64_t           hash  = 0;
	if (small) {
		std::array<uint8_t, SmallImageBytes> bytes;
		if (LibKernel::Memory::ReadGuestMemory(image.info.data.address, bytes.data(), image.info.data.size)) {
			hash = XXH3_64bits(bytes.data(), static_cast<size_t>(image.info.data.size));
			if (image.HasUploadHash() && image.UploadHash() == hash) {
				image.RefreshComplete();
				return;
			}
		}
	}
	InitializeImage(id);
	if (auto* refreshed = m_slot_images.try_get(id); refreshed != nullptr) {
		refreshed->SetUploadHash(hash, small && hash != 0);
	}
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	if (record.depth_id != depth_id || record.stencil_subresources != depth.stencil_subresources) {
		ImageInfo plane {};
		plane.data         = stencil;
		plane.pixel_format = vk::Format::eS8Uint;
		plane.guest_format = Prospero::BufferFormat::k8UInt;
		plane.type         = depth.info.type;
		plane.extent       = depth.info.extent;
		plane.resources    = {depth.stencil_subresources.level_count,
		                      depth.stencil_subresources.layer_count};
		plane.tile_mode    = depth.info.tile_mode;
		plane.pitch =
		    plane.IsTiled() ? TileGetDepthPitch(plane.extent.width, 1, 0) : depth.info.pitch;
		plane.bytes_per_block = 1;
		plane.UpdateSize();
		record.info                 = plane;
		record.depth_id             = depth_id;
		record.stencil_subresources = depth.stencil_subresources;
	}

	return association;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		std::scoped_lock lock {m_lock};
		return GetNullImage(desc);
	}
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

		for (const auto id: candidates) {
			const auto& image = m_slot_images[id];
			// Stencil descriptors name the associated plane; its depth owner supplies
			// the native image and the canonical guest tiling.
			if ((desc.type == BindingType::Texture || desc.type == BindingType::Storage) &&
			    image.depth_id && image.info.data == desc.info.data &&
			    m_slot_images[image.depth_id].info.samples == desc.info.samples &&
			    image.info.extent == desc.info.extent && image.info.type == desc.info.type &&
			    image.info.resources.levels >= desc.info.resources.levels &&
			    image.info.resources.layers >= desc.info.resources.layers &&
			    desc.info.bytes_per_block == 1 &&
			    desc.info.guest_format == Prospero::BufferFormat::k8UInt) {
				result = id;
				break;
			}
			if (SameBacking(image.info, desc.info, exact_format)) {
				result = id;
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		}

		if (result && !m_slot_images[result].depth_id) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				FreeImage(result);
				result = {};
			}
		}
		// An image first seen as a texture, storage image or display buffer was created unscaled;
		// drawn into later with scaled attachments, the pass shrank to the smallest one and showed
		// only the top-left of the scene (every other frame when buffers alternate). Recreate it at
		// the target scale the first time it becomes a render or depth target.
		if (result && view_mip < 0 && view_layer < 0 &&
		    (desc.type == BindingType::RenderTarget || desc.type == BindingType::DepthTarget)) {
			auto&       cached = m_slot_images[result];
			const float scale  = ChooseScale(desc);
			if (!cached.depth_id && cached.scale == 1.0f && scale != 1.0f &&
			    cached.info.extent == desc.info.extent && cached.info.data == desc.info.data &&
			    cached.backing.samples == 1) {
				const auto old_id = result;
				RefreshImage(old_id);
				const auto upgraded_id = InsertImage(m_slot_images[old_id].info, scale);
				auto&      upgraded    = m_slot_images[upgraded_id];
				auto&      old         = m_slot_images[old_id];
				upgraded.usage         = old.usage;
				if (old.binding.is_bound || old.binding.is_target) {
					old.binding.needs_rebind = true;
				}
				CopyImage(upgraded_id, old_id);
				FreeImage(old_id);
				result = upgraded_id;
			}
		}
		if (!result) {
			result         = InsertImage(desc.info, ChooseScale(desc));
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		}
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
	}
	if (!image.info.data.Empty()) {
		RefreshImage(id);
		if (image.info.HasStencil() &&
		    desc.info.data.address >= image.info.stencil.address &&
		    desc.info.data.End() <= image.info.stencil.End()) {
			for (const auto stencil_id:
			     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
				const auto* stencil = m_slot_images.try_get(stencil_id);
				if (stencil != nullptr && stencil->depth_id == id &&
				    stencil->info.data == image.info.stencil) {
					RefreshImage(stencil_id);
					break;
				}
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (!image.info.data.Empty()) {
				CommitGpuWrite(image);
			}
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.depth_target = true;
	if (desc.info.HasStencil()) {
		const auto layers = image.info.resources.layers;
		EXIT_IF(layers == 0 || image.info.data.size % layers != 0 ||
		        desc.info.data.address < image.info.data.address);
		const auto slice_size = image.info.data.size / layers;
		const auto offset     = desc.info.data.address - image.info.data.address;
		EXIT_IF(slice_size == 0 || offset % slice_size != 0 || offset / slice_size >= layers ||
		        desc.info.resources.layers > layers - offset / slice_size);
		image.stencil_subresources = {0, desc.info.resources.levels,
		                             static_cast<uint32_t>(offset / slice_size),
		                             desc.info.resources.layers};
	}
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		const auto metadata =
		    m_surface_metas.emplace(desc.info.metadata.range.address,
		                            MetaDataInfo {.type       = MetaDataInfo::Type::HTile,
		                                          .clear_mask = image.info.htile_clear_mask}).first;
		metadata->second.owner = id;
		if (desc.info.metadata.gfx7_htile != 0) {
			m_has_gfx7_htile.store(true, std::memory_order_relaxed);
			Common::DebugFlags::NoteOnce("CapyPS5-Htile", desc.info.metadata.range.address ^ 2u,
			                             "PS4 depth 0x%llx %ux%u has HTile 0x%llx (%llu bytes/slice, addressing %u)",
			                             static_cast<unsigned long long>(desc.info.data.address),
			                             desc.info.extent.width, desc.info.extent.height,
			                             static_cast<unsigned long long>(desc.info.metadata.range.address),
			                             static_cast<unsigned long long>(desc.info.metadata.gfx7_htile_slice),
			                             desc.info.metadata.gfx7_htile - 1u);
		}
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (Common::DebugFlags::VideoProbeEnabled() && image.info.extent.width == 1920 &&
	    image.info.extent.height == 1080 && image.info.guest_format == Prospero::BufferFormat::k8_8_8_8UNorm) {
		Common::DebugFlags::NoteOnce("CapyPS5-VideoProbe", image.info.data.address ^ 2u,
		    "GPU write image=0x%llx", static_cast<unsigned long long>(image.info.data.address));
	}
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	Common::DebugFlags::WatchWrite("image write", image.info.data.address, image.info.data.size,
	                               static_cast<uint64_t>(image.info.guest_format));
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

// Whether a buffer fill of `size` bytes at an image's address clears the whole image. PS4 (gfx7)
// images are laid out here with gfx10 sizes, larger than the guest's: a fill of the guest's real
// footprint covers every texel without reaching data.size. God of War Ragnarok clears its stencil
// plane so; as a buffer write it was then uploaded into the depth/stencil image by a transfer,
// which left the stock Adreno driver's LRZ stale and dropped later fragments in 8x8 blocks.
static bool FillCoversImage(const ImageInfo& info, uint64_t size) {
	if (size == info.data.size) {
		return true;
	}
	if (ShaderRecompiler::Decoder::GetGuestIsa() != ShaderRecompiler::Decoder::GuestIsa::Gfx7 ||
	    size > info.data.size || info.resources.levels != 1 || info.resources.layers != 1) {
		return false;
	}
	const uint64_t texels = uint64_t {info.extent.width} * info.extent.height * info.extent.depth;
	return size >= texels * info.bytes_per_block;
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock      lock {m_lock};
	ImageId               selected {};
	ImageId               stencil_id {};
	vk::ImageAspectFlags  aspect {};
	ImageSubresourceRange subresources;
	Common::DebugFlags::FrameTrace("  image clear 0x%llx size=%llu value=0x%08x",
	                               static_cast<unsigned long long>(address),
	                               static_cast<unsigned long long>(size), packed_clear);
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		Common::DebugFlags::FrameTrace("    candidate addr=0x%llx size=%llu %ux%u vkfmt=%d depth=%d",
		                               static_cast<unsigned long long>(owner->info.data.address),
		                               static_cast<unsigned long long>(owner->info.data.size),
		                               owner->info.extent.width, owner->info.extent.height,
		                               static_cast<int>(owner->info.pixel_format), owner->depth_id ? 1 : 0);
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		ImageSubresourceRange candidate_subresources {
		    0, owner->info.resources.levels, 0, owner->info.TransferLayers()};
		if (owner->depth_id && owner->info.data.address == address &&
		    FillCoversImage(owner->info, size)) {
			candidate              = vk::ImageAspectFlagBits::eStencil;
			candidate_id           = owner->depth_id;
			candidate_subresources = owner->stencil_subresources;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           FillCoversImage(owner->info, size)) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && (selected != candidate_id || subresources != candidate_subresources)) {
			return false;
		}
		selected     = candidate_id;
		stencil_id   = candidate == vk::ImageAspectFlagBits::eStencil ? id : ImageId {};
		aspect       = candidate;
		subresources = candidate_subresources;
	}
	if (!selected) {
		Common::DebugFlags::FrameTrace("    -> no exact image, dispatch runs");
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			Common::DebugFlags::FrameTrace("    -> undecodable fill, dispatch runs");
			return false;
		}
		Common::DebugFlags::FrameTrace("    -> cleared color %f %f %f %f", clear.color.float32[0],
		                               clear.color.float32[1], clear.color.float32[2],
		                               clear.color.float32[3]);
	} else {
		// D16 buffer fills must clear the Vulkan image as well as its guest backing.
		// debug.capy.d16_clear=0 restores the fallback fill for diagnosis.
		static Common::DebugFlags::CachedValue d16_clear("debug.capy.d16_clear", 1);
		if (aspect == vk::ImageAspectFlagBits::eDepth &&
		    DepthAspectTransferFormat(image.info.pixel_format) == vk::Format::eD16Unorm &&
		    d16_clear.Get() == 0) {
			return false;
		}
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, subresources.base_level, subresources.level_count,
	            subresources.base_layer, subresources.layer_count}, clear);
	if (stencil_id) {
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	Common::DebugFlags::FrameTrace("  tc %s id=%u addr=0x%llx", "clear", static_cast<uint32_t>(id.index),
	                               static_cast<unsigned long long>(m_slot_images[id].info.data.address));
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.backing.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.backing.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		Common::DebugFlags::LogDepthWrite("ClearDepthStencilImage", image.track_addr);
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.linear_slice_stride;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			const auto copies = info.BufferCopies(destination_offset);
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
		} else {
			const auto copies = info.BufferCopies();
			m_tiler.TileImage(image, copies, destination.Handle(), destination_offset,
			                  info.data.size);
		}
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	const std::array copies {vk::BufferImageCopy {0,
	                                              info.pitch,
	                                              info.extent.height,
	                                              {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                                              {},
	                                              info.extent}};
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto guest_linear = tiled ? m_tiler.GetScratchBuffer(info.LinearSize(), host_linear.buffer)
	                          : TileManager::Result {destination.Handle(), destination_offset,
	                                                 destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (tiled) {
		m_tiler.Tile(guest_linear.buffer, guest_linear.offset, guest_linear.size,
		             destination.Handle(), destination_offset, info.data.size, info);
	}
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                 uint64_t destination_size, uint32_t levels) {
	if (!CanDownload(image)) {
		EXIT("TextureCache: invalid image download\n");
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		EXIT_IF(destination_size != image.info.data.size || levels != image.info.resources.levels);
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	const auto& info      = image.info;
	const auto  transform = info.GetColorTransform();
	if (info.IsTiled()) {
		const auto copies = info.BufferCopies(0, levels);
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, destination_size,
		                  levels);
	} else if (transform != ColorTransform::None) {
		auto       linear = m_tiler.GetScratchBuffer(destination_size);
		const auto copies = info.BufferCopies(0, levels);
		image.Download(copies, linear.buffer, 0, linear.size);
		m_tiler.TransformColor(linear, {destination.Handle(), destination_offset, destination_size},
		                       transform, false);
	} else {
		const auto copies = info.BufferCopies(destination_offset, levels);
		image.Download(copies, destination.Handle(), destination_offset, destination_size);
	}
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else if (image.info.IsVolume() || image.info.resources.layers > 1) {
		// Array and volume slices are strided across the full mip chain.
		return false;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	if (!m_texture_cache.CanDownload(image) ||
	    (TextureCache::UploadBinding(image) == TextureCache::BindingType::DepthTarget &&
	     copy_size != image.info.data.size)) {
		return false;
	}
	Common::DebugFlags::NoteOnce("CapyPS5-BufImg", image.info.data.address ^ 7u,
	                             "buffer read 0x%llx+%llu downloads image 0x%llx %ux%u fmt=%u tile=%u "
	                             "gfx7size=%llu scaled=%d",
	                             static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size),
	                             static_cast<unsigned long long>(image.info.data.address),
	                             image.info.extent.width, image.info.extent.height,
	                             static_cast<uint32_t>(image.info.guest_format),
	                             static_cast<uint32_t>(image.info.tile_mode),
	                             static_cast<unsigned long long>(image.info.gfx7_guest_size),
	                             image.IsScaled() ? 1 : 0);
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
	return true;
}

bool BufferCache::SynchronizeGfx7Htile(Buffer* buffer, uint64_t vaddr, uint64_t size) {
	if (ShaderRecompiler::Decoder::GetGuestIsa() != ShaderRecompiler::Decoder::GuestIsa::Gfx7) {
		return false;
	}
	// `debug.capy.gfx7_htile_gen 0`: leave PS4 HTiles as the game's clears left them.
	static Common::DebugFlags::CachedValue gfx7_htile_gen("debug.capy.gfx7_htile_gen", 1);
	// `debug.capy.gfx7_htile_addr <1 + addressing>`: force the HTile layout (diagnosis).
	static Common::DebugFlags::CachedValue gfx7_htile_addr("debug.capy.gfx7_htile_addr", 0);
	// `debug.capy.gfx7_htile_z <1 + 14-bit depth>`: write that depth to every tile (diagnosis).
	static Common::DebugFlags::CachedValue gfx7_htile_z("debug.capy.gfx7_htile_z", 0);
	if (gfx7_htile_gen.Get() == 0) {
		return false;
	}
	// Texture-cache paths that obtain buffers (CopyToImage) already hold its lock.
	if (!m_texture_cache.m_has_gfx7_htile.load(std::memory_order_relaxed) ||
	    m_texture_cache.m_lock.HeldByCurrentThread()) {
		return false;
	}
	std::scoped_lock lock {m_texture_cache.m_lock};
	auto&            metas = m_texture_cache.m_surface_metas;
	if (metas.empty()) {
		return false;
	}
	bool found = false;
	auto it    = metas.upper_bound(vaddr);
	if (it != metas.begin()) {
		--it;
	}
	for (; it != metas.end() && it->first < vaddr + size; ++it) {
		const auto& meta = it->second;
		if (meta.type != TextureCache::MetaDataInfo::Type::HTile || !meta.owner) {
			continue;
		}
		auto&       image = m_texture_cache.m_slot_images[meta.owner];
		const auto& info  = image.info;
		if (!image.registered || image.depth_id || info.metadata.gfx7_htile == 0 ||
		    info.metadata.range.address != it->first) {
			continue;
		}
		const auto htile_size = info.metadata.gfx7_htile_slice * info.resources.layers;
		if (it->first + htile_size <= vaddr) {
			continue;
		}
		// A binding of a whole heap that only contains the HTile does not read it.
		if (size > htile_size * 2 + 0x10000) {
			Common::DebugFlags::NoteOnce("CapyPS5-Htile", it->first ^ 1u,
			                             "HTile 0x%llx inside a %llu-byte binding: not rebuilt",
			                             static_cast<unsigned long long>(it->first),
			                             static_cast<unsigned long long>(size));
			continue;
		}
		const auto transfer_bytes = DepthAspectTransferBytes(info.pixel_format);
		if (!image.IsGpuModified() || info.samples != 1 ||
		    image.backing.samples != 1 || image.backing.image == nullptr || transfer_bytes == 0) {
			continue;
		}
		found = true;
		if (buffer == nullptr) {
			break;
		}
		if (!buffer->IsInBounds(it->first, 4)) {
			continue;
		}
		// The host image may be scaled (debug.capy.res_scale): read it at its own size.
		const auto layers       = info.resources.layers;
		const auto host_extent  = image.backing.extent;
		const auto host_pitch   = image.IsScaled() ? host_extent.width : info.pitch;
		const auto source_pitch = static_cast<uint64_t>(host_pitch) * transfer_bytes;
		const auto source_slice = source_pitch * host_extent.height;
		auto       scratch      = m_texture_cache.m_tiler.GetScratchBuffer(source_slice * layers);
		const std::array copies {vk::BufferImageCopy {scratch.offset,
		                                              host_pitch,
		                                              host_extent.height,
		                                              {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
		                                              {},
		                                              {host_extent.width, host_extent.height, 1}}};
		image.DownloadBacking(copies, scratch.buffer, scratch.offset, scratch.size);
		const auto format   = DepthAspectTransferFormat(info.pixel_format);
		const auto encoding = transfer_bytes == 2 ? 2u : (format == vk::Format::eD32Sfloat ? 0u : 1u);
		const auto offset   = buffer->Offset(it->first);
		m_texture_cache.m_tiler.GenerateGfx7Htile(
		    scratch, {buffer->Handle(), offset, std::min(htile_size, buffer->Size() - offset)},
		    {.width        = info.extent.width,
		     .height       = info.extent.height,
		     .host_width   = host_extent.width,
		     .host_height  = host_extent.height,
		     .layers       = layers,
		     .source_pitch = source_pitch,
		     .source_slice = source_slice,
		     .htile_slice  = info.metadata.gfx7_htile_slice,
		     .tiles_x      = info.metadata.gfx7_htile_tiles_x,
		     .addressing   = gfx7_htile_addr.Get() > 0 ? static_cast<uint32_t>(gfx7_htile_addr.Get() - 1)
		                                                : info.metadata.gfx7_htile - 1u,
		     .encoding     = encoding,
		     .fixed_z      = static_cast<int32_t>(gfx7_htile_z.Get()) - 1});
		Common::DebugFlags::NoteOnce("CapyPS5-Htile", it->first,
		                             "rebuilt HTile 0x%llx (%llu bytes, %ux%u depth, addressing %u)",
		                             static_cast<unsigned long long>(it->first),
		                             static_cast<unsigned long long>(htile_size), info.extent.width,
		                             info.extent.height, info.metadata.gfx7_htile - 1u);
	}
	return found;
}

bool TextureCache::DebugSaveImageLinear(ImageId id, const std::string& path) {
	std::scoped_lock lock {m_lock};
	auto& image = m_slot_images[id];
	if (image.depth_id || image.backing.image == nullptr || image.info.samples != 1) {
		return false;
	}
	// A scaled image is saved at its host size (backing.extent).
	const auto     extent = image.IsScaled() ? image.backing.extent : image.info.extent;
	const uint32_t width  = extent.width;
	// Layers and volume slices follow one another in the dump, as extra rows.
	const uint32_t slices = image.info.TransferLayers();
	const uint32_t height = extent.height * slices;
	const bool     is_depth = image.backing.format == vk::Format::eD32Sfloat ||
	                      image.backing.format == vk::Format::eD32SfloatS8Uint;
	const uint32_t bpp    = is_depth ? 4u : image.info.bytes_per_block;
	if (width == 0 || height == 0 || bpp == 0 || bpp > 16) {
		return false;
	}
	const uint64_t size     = static_cast<uint64_t>(width) * height * bpp;
	auto&          download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset]   = download.Map(size, 16);
	if (mapped == nullptr) {
		return false;
	}
	download.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset      = offset;
	copy.imageSubresource  = {is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 0,
	                          image.info.IsVolume() ? 1u : slices};
	copy.imageExtent       = vk::Extent3D {width, extent.height, image.info.IsVolume() ? slices : 1u};
	image.DownloadBacking(std::span {&copy, 1}, download.Handle(), offset, size);
	const auto format = static_cast<uint32_t>(image.backing.format);
	m_scheduler.DeferPriorityOperation([&download, mapped, offset, size, width, height, format, bpp,
	                                    path] {
		download.Invalidate(offset, size);
		if (auto* out = std::fopen(path.c_str(), "wb"); out != nullptr) {
			const uint32_t header[4] = {width, height, format, bpp};
			std::fwrite("CAPYIMG1", 1, 8, out);
			std::fwrite(header, sizeof(header), 1, out);
			std::fwrite(mapped, 1, size, out);
			std::fclose(out);
		}
	});
	return true;
}

int64_t TextureCache::NanHuntMode() {
	static Common::DebugFlags::CachedValue nan_hunt("debug.capy.nan_hunt", 0);
	static int64_t                         armed_value = 0;
	static uint64_t                        hunt_frame  = 0;
	const auto                             value       = nan_hunt.Get();
	const auto frame = Common::Telemetry::g_game_frames.load(std::memory_order_relaxed);
	if (value != armed_value) {
		armed_value = value;
		// The frame after the current one, so the hunt covers a whole frame from its start.
		hunt_frame = value != 0 ? frame + 1 : 0;
		if (value != 0) {
			Common::DebugFlags::FrameTrace("nan hunt armed for frame %llu",
			                               static_cast<unsigned long long>(hunt_frame));
#if defined(__ANDROID__)
			__android_log_print(ANDROID_LOG_INFO, "CapyPS5-NanHunt", "armed (mode %lld) for frame %llu",
			                    static_cast<long long>(value), static_cast<unsigned long long>(hunt_frame));
#endif
		}
	}
	return hunt_frame != 0 && frame == hunt_frame ? armed_value : 0;
}

namespace {

// Counts NaN and infinite components of `count` texels in a host format; false when the format
// holds no floats.
bool CountNonFinite(vk::Format format, const uint8_t* data, uint64_t count, uint64_t& nans,
                    uint64_t& infs, uint64_t& first) {
	uint32_t components = 0;
	uint32_t bits       = 0;
	switch (format) {
		case vk::Format::eR16Sfloat: components = 1; bits = 16; break;
		case vk::Format::eR16G16Sfloat: components = 2; bits = 16; break;
		case vk::Format::eR16G16B16A16Sfloat: components = 4; bits = 16; break;
		case vk::Format::eR32Sfloat:
		case vk::Format::eD32Sfloat: components = 1; bits = 32; break;
		case vk::Format::eR32G32Sfloat: components = 2; bits = 32; break;
		case vk::Format::eR32G32B32A32Sfloat: components = 4; bits = 32; break;
		case vk::Format::eB10G11R11UfloatPack32: components = 3; bits = 11; break;
		default: return false;
	}
	const auto note = [&](uint64_t texel, bool nan) {
		if (nans + infs == 0) {
			first = texel;
		}
		(nan ? nans : infs)++;
	};
	for (uint64_t t = 0; t < count; t++) {
		if (bits == 16) {
			const auto* h = reinterpret_cast<const uint16_t*>(data) + t * components;
			for (uint32_t c = 0; c < components; c++) {
				if ((h[c] & 0x7c00u) == 0x7c00u) {
					note(t, (h[c] & 0x03ffu) != 0);
				}
			}
		} else if (bits == 32) {
			const auto* w = reinterpret_cast<const uint32_t*>(data) + t * components;
			for (uint32_t c = 0; c < components; c++) {
				if ((w[c] & 0x7f800000u) == 0x7f800000u) {
					note(t, (w[c] & 0x007fffffu) != 0);
				}
			}
		} else {
			uint32_t packed = 0;
			std::memcpy(&packed, data + t * 4, 4);
			// R and G: 6-bit mantissa, 5-bit exponent; B: 5-bit mantissa, 5-bit exponent.
			const uint32_t fields[3][2] = {{packed & 0x7ffu, 6}, {(packed >> 11u) & 0x7ffu, 6},
			                               {(packed >> 22u) & 0x3ffu, 5}};
			for (const auto& [field, mantissa_bits]: fields) {
				if ((field >> mantissa_bits) == 0x1fu) {
					note(t, (field & ((1u << mantissa_bits) - 1u)) != 0);
				}
			}
		}
	}
	return true;
}

} // namespace

void TextureCache::DebugScanImageNan(ImageId id, const std::string& label) {
	std::scoped_lock lock {m_lock};
	auto& image = m_slot_images[id];
	if (image.depth_id || image.backing.image == nullptr || image.info.samples != 1 ||
	    image.info.IsVolume()) {
		return;
	}
	const auto format = image.backing.format;
	uint64_t   probe_nans = 0, probe_infs = 0, probe_first = 0;
	if (!CountNonFinite(format, nullptr, 0, probe_nans, probe_infs, probe_first)) {
		return;
	}
	const auto     extent = image.IsScaled() ? image.backing.extent : image.info.extent;
	const uint32_t bpp    = vk::blockSize(format);
	const uint64_t size   = static_cast<uint64_t>(extent.width) * extent.height * bpp;
	if (size == 0) {
		return;
	}
	auto& download        = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(size, 16);
	if (mapped == nullptr) {
		return;
	}
	download.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset     = offset;
	copy.imageSubresource = {format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth
	                                                          : vk::ImageAspectFlagBits::eColor,
	                         0, 0, 1};
	copy.imageExtent      = vk::Extent3D {extent.width, extent.height, 1};
	image.DownloadBacking(std::span {&copy, 1}, download.Handle(), offset, size);
	const auto address = image.info.data.address;
	const auto width   = extent.width;
	const auto height  = extent.height;
	// `debug.capy.nan_roi (x0 << 16 | y0)` and `debug.capy.nan_roi_end (x1 << 16 | y1)`: also count
	// the non-finite texels inside that rectangle (the log's roi=), to find the draw that should
	// cover it.
	static Common::DebugFlags::CachedValue nan_roi("debug.capy.nan_roi", 0);
	static Common::DebugFlags::CachedValue nan_roi_end("debug.capy.nan_roi_end", 0);
	const auto roi     = static_cast<uint64_t>(std::max<int64_t>(nan_roi.Get(), 0));
	const auto roi_end = static_cast<uint64_t>(std::max<int64_t>(nan_roi_end.Get(), 0));
	m_scheduler.DeferPriorityOperation([&download, mapped, offset, size, format, width, height,
	                                    address, label, roi, roi_end] {
		download.Invalidate(offset, size);
		uint64_t nans = 0, infs = 0, first = 0;
		CountNonFinite(format, static_cast<const uint8_t*>(mapped),
		               static_cast<uint64_t>(width) * height, nans, infs, first);
		if (nans + infs == 0) {
			return;
		}
		uint64_t roi_count = 0;
		if (roi_end != 0) {
			const uint32_t x0 = roi >> 16u, y0 = roi & 0xffffu;
			const uint32_t x1 = std::min<uint32_t>(roi_end >> 16u, width), y1 = std::min<uint32_t>(roi_end & 0xffffu, height);
			const uint32_t bpp = vk::blockSize(format);
			for (uint32_t y = y0; y < y1; y++) {
				uint64_t row_nans = 0, row_infs = 0, row_first = 0;
				if (x1 > x0) {
					CountNonFinite(format, static_cast<const uint8_t*>(mapped) + (uint64_t {y} * width + x0) * bpp,
					               x1 - x0, row_nans, row_infs, row_first);
				}
				roi_count += row_nans + row_infs;
			}
		}
		Common::DebugFlags::FrameTrace("  NAN %s image=0x%llx %ux%u nan=%llu inf=%llu", label.c_str(),
		                               static_cast<unsigned long long>(address), width, height,
		                               static_cast<unsigned long long>(nans),
		                               static_cast<unsigned long long>(infs));
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-NanHunt",
		                    "%s image=0x%llx %ux%u vkfmt=%u nan=%llu inf=%llu first=(%llu,%llu) roi=%llu",
		                    label.c_str(), static_cast<unsigned long long>(address), width, height,
		                    static_cast<uint32_t>(format), static_cast<unsigned long long>(nans),
		                    static_cast<unsigned long long>(infs),
		                    static_cast<unsigned long long>(first % width),
		                    static_cast<unsigned long long>(first / width),
		                    static_cast<unsigned long long>(roi_count));
#endif
	});
}

bool TextureCache::DownloadImageMemory(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id) {
		return false;
	}
	if (!image.SafeToDownload() || !CanDownload(image)) {
		return false;
	}
	const auto range = image.info.data;
	auto download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download)
	                    .MapDownload(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4));
	if (!LibKernel::Memory::TryReadBacking(range.address, download.mapped, range.size)) {
		return false;
	}
	download.buffer->Flush(download.offset, range.size);

	DownloadImage(image, *download.buffer, download.offset, range.size,
	              image.info.resources.levels);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.buffer->Handle();
	barrier.offset              = download.offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([download = std::move(download), range] {
		download.buffer->Invalidate(download.offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, download.mapped, range.size);
	});
	return true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (image.info.data.address != address) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

bool TextureCache::CopyToImage(uint64_t address, uint64_t source, uint64_t size) {
	std::scoped_lock lock {m_lock};
	const auto       targets = FindImagesInRegion(address, size, false);
	if (targets.size() != 1) {
		return false;
	}
	auto&       image   = m_slot_images[targets.front()];
	const auto& info    = image.info;
	const auto  sources = FindImagesInRegion(source, size, false);
	// PS4 (gfx7) layouts do not follow the gfx10 mip layout this walks.
	const bool gfx7_layout = info.gfx7_micro_pitch != 0 || info.gfx7_guest_size != 0;
	if (gfx7_layout || image.depth_id || !image.SafeToDownload() || !CanDownload(image) ||
	    !info.IsTiled() || info.IsVolume() || info.IsDepth() || info.HasMetadata() ||
	    address < info.data.address ||
	    std::any_of(sources.begin(), sources.end(),
	                [&](ImageId id) { return m_slot_images[id].IsGpuModified(); })) {
		return false;
	}
	// Each layer holds its levels from the smallest up, and the tail levels share one block.
	const auto layers  = info.resources.layers;
	const auto stride  = info.data.size / layers;
	const auto offset  = address - info.data.address;
	const auto begin   = offset % stride;
	const auto end     = begin + size;
	uint64_t   covered = begin;
	uint32_t   first   = 0;
	uint32_t   count   = 0;
	for (uint32_t level = info.resources.levels; level-- > 0;) {
		const auto& mip     = info.mip_layout[level];
		const auto  mip_end = mip.offset + mip.size / layers;
		if (mip_end <= begin || mip.offset >= end) {
			continue;
		}
		if (mip.offset < begin) {
			return false;
		}
		covered = mip_end;
		first   = level;
		count++;
	}
	if (covered != end) {
		return false;
	}
	ImageInfo  part   = info;
	const auto extent = info.MipExtent(first);
	part.data.size    = size;
	part.extent       = {extent.width, extent.height, 1};
	part.resources    = {count, 1};
	part.first_tail_level -= std::min(first, info.first_tail_level);
	uint64_t linear = 0;
	for (uint32_t level = 0; level < count; level++) {
		auto& mip = part.mip_layout[level] = info.mip_layout[first + level];
		mip.offset -= begin;
		mip.size /= layers;
		mip.linear_offset = Common::AlignUp<uint64_t>(linear, std::max(info.bytes_per_block, 4u));
		linear            = mip.linear_offset + mip.linear_size;
	}
	const auto [buffer, source_offset] = m_buffer_cache.ObtainBuffer(source, size, false);
	const auto texels = m_tiler.Detile(buffer->Handle(), source_offset, part);
	auto       copies = part.BufferCopies(texels.offset);
	for (auto& copy: copies) {
		copy.imageSubresource.mipLevel += first;
		copy.imageSubresource.baseArrayLayer = static_cast<uint32_t>(offset / stride);
	}
	image.Upload(copies, texels.buffer, texels.offset, texels.size);
	return true;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			const auto real_size = owner->info.gfx7_guest_size;
			if (real_size != 0 && real_size < owner->info.data.size &&
			    address >= owner->info.data.address + real_size) {
				// Past the end of a PS4 target: the game's own next allocation, not the image.
				// Stop watching those pages so the write can proceed.
				const auto keep_end =
				    Common::AlignDown(owner->info.data.address + real_size, TRACKER_PAGE_SIZE);
				if (owner->IsTracked() && owner->track_addr_end > keep_end &&
				    owner->track_addr < keep_end) {
					const auto old_end    = owner->track_addr_end;
					owner->track_addr_end = keep_end;
					m_page_manager.UpdatePageWatchers<false>(keep_end, old_end - keep_end);
				} else if (owner->IsTracked() && owner->track_addr >= keep_end) {
					UntrackImage(id);
				}
				continue;
			}
			owner->InvalidateCpuWrite(address, size);
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::HasImagesInRegion(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	return (found->second.clear_mask & (1u << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= 1u << slice;
	} else {
		found->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

bool TextureCache::IsRegionRegistered(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	Common::DebugFlags::FrameTrace("  tc gc used=%llu trigger=%llu", static_cast<unsigned long long>(m_total_used_memory), static_cast<unsigned long long>(m_trigger_gc_memory));
	const uint64_t   tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	// `setprop debug.capy.gc_trigger_mb N` overrides the collection threshold (testing).
	static Common::DebugFlags::CachedValue gc_trigger_mb("debug.capy.gc_trigger_mb", 0);
	const uint64_t trigger = gc_trigger_mb.Get() > 0
	                             ? static_cast<uint64_t>(gc_trigger_mb.Get()) * 1024u * 1024u
	                             : m_trigger_gc_memory;
	if (m_total_used_memory < trigger) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first. Kept images do not use up the deletion budget.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) { candidates.push_back(id); });
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			// GPU-written images, tiled ones included, are written back under memory pressure
			// only; stencil-backed ones stay, and so do tiled PS4 (gfx7) layouts, which the
			// gfx10 tiler cannot write back.
			const bool gfx7_tiled = owner->info.IsTiled() && (owner->info.gfx7_micro_pitch != 0 ||
			                                                  owner->info.gfx7_guest_size != 0);
			if (owner->SafeToDownload() && (!pressured || owner->info.HasStencil() || gfx7_tiled ||
			                                !DownloadImageMemory(id))) {
				continue;
			}
			--deletions;
			FreeImage(id);
			if (m_total_used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (m_total_used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (m_total_used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

void TextureCache::ProcessDownloadImages() {
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
