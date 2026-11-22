// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/depthRenderTarget.h"

#include "common/assert.h"
#include "common/alignment.h"
#include "common/common.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace Libs::Graphics {

[[noreturn]] static void DepthFatal(const char* format, ...) {
	char    reason[256];
	va_list args;
	va_start(args, format);
	std::vsnprintf(reason, sizeof(reason), format, args);
	va_end(args);
	EXIT("unsupported depth render state: %s\n", reason);
}

static vk::StencilOp ConvertStencilOp(uint8_t value, uint8_t write_mask, uint8_t op_value) {
	if (write_mask == 0) {
		return vk::StencilOp::eKeep;
	}
	switch (static_cast<Prospero::StencilOp>(value)) {
		case Prospero::StencilOp::kKeep: return vk::StencilOp::eKeep;
		case Prospero::StencilOp::kZero: return vk::StencilOp::eZero;
		case Prospero::StencilOp::kReplaceTest: return vk::StencilOp::eReplace;
		case Prospero::StencilOp::kReplaceOp:
			if ((op_value & write_mask) == 0) {
				return vk::StencilOp::eZero;
			}
			return vk::StencilOp::eReplace;
		case Prospero::StencilOp::kAddClamp: return vk::StencilOp::eIncrementAndClamp;
		case Prospero::StencilOp::kSubClamp: return vk::StencilOp::eDecrementAndClamp;
		case Prospero::StencilOp::kInvert: return vk::StencilOp::eInvert;
		case Prospero::StencilOp::kAddWrap: return vk::StencilOp::eIncrementAndWrap;
		case Prospero::StencilOp::kSubWrap: return vk::StencilOp::eDecrementAndWrap;
		case Prospero::StencilOp::kXor:
			if ((write_mask & op_value) == 0) {
				return vk::StencilOp::eKeep;
			}
			if ((write_mask & ~op_value) != 0) {
				DepthFatal("unsupported stencil XOR operands: write mask=0x%02" PRIx8
				           ", operation value=0x%02" PRIx8,
				           write_mask, op_value);
			}
			return vk::StencilOp::eInvert;
		default: DepthFatal("unsupported stencil operation: 0x%02" PRIx8, value);
	}
}

static vk::StencilOpState ConvertStencilState(
    uint8_t compare, const std::array<uint8_t, 3>& operations, uint8_t op_value,
    const vk::StencilOpState& state) {
	const auto test_value = state.reference;
	auto reference       = test_value;
	auto required_bits   = state.compareMask;
	if (compare == static_cast<uint8_t>(vk::CompareOp::eAlways) ||
	    compare == static_cast<uint8_t>(vk::CompareOp::eNever)) {
		required_bits = 0;
	}
	std::array<vk::StencilOp, 3> converted {};
	for (size_t i = 0; i < operations.size(); i++) {
		converted[i] = ConvertStencilOp(operations[i], state.writeMask, op_value);
		if (converted[i] != vk::StencilOp::eReplace) {
			continue;
		}
		auto replacement = test_value;
		if (static_cast<Prospero::StencilOp>(operations[i]) == Prospero::StencilOp::kReplaceOp) {
			replacement = op_value;
		}
		if (((reference ^ replacement) & required_bits & state.writeMask) != 0) {
			DepthFatal("unsupported stencil replacement: compare=%u, compare mask=0x%02" PRIx32
			           ", write mask=0x%02" PRIx32 ", operation value=0x%02" PRIx8
			           ", test value=0x%02" PRIx32,
			           compare, state.compareMask, state.writeMask, op_value, test_value);
		}
		// Vulkan shares one reference between comparison and every replacement on this face.
		reference = (reference & ~state.writeMask) | (replacement & state.writeMask);
		required_bits |= state.writeMask;
	}
	return {converted[0], converted[1], converted[2], static_cast<vk::CompareOp>(compare),
	        state.compareMask, state.writeMask, reference};
}

// Without a policy (no Z surface), the target is a stencil-only S8 attachment.
[[nodiscard]] static vk::Format ResolveHostDepthAttachmentFormat(const CommandBuffer&     buffer,
                                                                 const DepthFormatPolicy* policy,
                                                                 bool      has_stencil,
                                                                 uint32_t* samples) {
	auto&      graphics = buffer.GetGraphics();
	const auto supports = [&](vk::Format format) {
		vk::ImageFormatProperties properties {};
		return format != vk::Format::eUndefined &&
		       graphics.GetImageFormatProperties(
		           format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, DepthTargetImageUsage(),
		           vk::ImageCreateFlags {}, &properties) == vk::Result::eSuccess &&
		       static_cast<bool>(properties.sampleCounts & vulkan_sample_count(*samples));
	};
	const auto try_formats = [&]() -> vk::Format {
		if (policy == nullptr) {
			return supports(vk::Format::eS8Uint) ? vk::Format::eS8Uint : vk::Format::eUndefined;
		}
		if (!has_stencil) {
			return supports(policy->depth_attachment_format) ? policy->depth_attachment_format
			                                                 : vk::Format::eUndefined;
		}
		for (const auto format: policy->stencil_attachment_formats) {
			if (supports(format)) {
				return format;
			}
		}
		return vk::Format::eUndefined;
	};
	auto format = try_formats();
	// The game's MSAA level can be beyond this GPU (8x on Adreno, for example); step down
	// before dropping the depth attachment (which would kill depth testing) or dying.
	while (format == vk::Format::eUndefined && *samples > 1) {
		*samples >>= 1;
		format = try_formats();
	}
	return format;
}

static TextureCache::ImageDesc MakeDepthTargetDesc(const CommandBuffer& buffer,
                                                  const HW::DepthRenderTarget& z,
                                                  bool write_buffer = false) {
	const bool has_depth   = z.z_info.format != Prospero::DepthFormat::kInvalid;
	const bool has_stencil = z.stencil_info.format != Prospero::StencilFormat::kInvalid;
	const auto depth_address = write_buffer ? z.z_write_base_addr : z.z_read_base_addr;
	const auto stencil_address =
	    write_buffer ? z.stencil_write_base_addr : z.stencil_read_base_addr;
	if (!z.z_info.HasValidTextureCompatibility() ||
	    !z.stencil_info.HasValidTextureCompatibility()) {
		DepthFatal("invalid PS5 depth texture-compatibility encoding");
	}
	// PS4 HTile buffers are only register (256-byte) aligned. The HTile serves clear tracking by
	// its base address here, so a PS4 one need not meet the PS5 32 KiB alignment.
	const bool gfx7_target =
	    ShaderRecompiler::Decoder::GetGuestIsa() == ShaderRecompiler::Decoder::GuestIsa::Gfx7;
	const bool has_htile = z.z_info.htile_acceleration;
	const bool unsupported_shading_rate_encoding =
	    z.shading_rate_encoding > 1 || (z.shading_rate_encoding != 0 && !has_htile);
	const auto guest_samples = render_sample_count(z.z_info.num_samples);
	if (guest_samples == 0) {
		DepthFatal("unsupported depth fragment count: %u", z.z_info.num_samples);
	}
	auto samples = guest_samples;
	const bool htile_stencil_compat = depth_htile_stencil_acceleration_compatible(
	    has_stencil, has_htile, z.stencil_info.htile_stencil_disabled);
	const auto view = ResolveTargetViewInfo(z.depth_view.slice_start, z.depth_view.slice_max);
	switch (view.type) {
		case TargetViewType::Image2D:
		case TargetViewType::Image2DArray: break;
		case TargetViewType::Unsupported:
			DepthFatal("invalid depth view: base=%u last=%u", z.depth_view.slice_start,
			           z.depth_view.slice_max);
	}
	// EXPCLEAR permits an HTile acceleration state; the host attachment is already expanded.
	// PS5 depth buffers are 64 KiB aligned; PS4 ones only to the 256-byte register granularity.
	const uint64_t base_align_mask =
	    ShaderRecompiler::Decoder::GetGuestIsa() == ShaderRecompiler::Decoder::GuestIsa::Gfx7 ? 0xffu : 0xffffu;
	if (z.z_info.max_mip_level != 0 || unsupported_shading_rate_encoding ||
	    (has_depth ? depth_address == 0 || (depth_address & base_align_mask) != 0 : !has_stencil)) {
		DepthFatal("unsupported depth target: mips=%u, shading rate=%u, htile=%d, address=0x%" PRIx64,
		           static_cast<unsigned>(z.z_info.max_mip_level), static_cast<unsigned>(z.shading_rate_encoding),
		           has_htile ? 1 : 0, depth_address);
	}
	if (has_stencil) {
		if (z.stencil_info.format != Prospero::StencilFormat::k8UInt || !htile_stencil_compat ||
		    stencil_address == 0 || (stencil_address & base_align_mask) != 0) {
			DepthFatal("unsupported stencil attachment state");
		}
	}
	if (has_htile) {
		if (z.htile_data_base_addr == 0 ||
		    (z.htile_data_base_addr & (gfx7_target ? 0xffu : 0x7fffu)) != 0) {
			DepthFatal("invalid HTile metadata address");
		}
		if (z.depth_view.slice_max >= 32) {
			DepthFatal("HTile clear tracking supports at most 32 slices");
		}
	}
	if (!z.size.valid) {
		DepthFatal("missing depth extent");
	}
	// With a single level, the selected mip is a surface of that mip's extent at the base address.
	const auto     mip    = z.depth_view.current_mip_level;
	const uint32_t width  = std::max((static_cast<uint32_t>(z.size.x_max) + 1u) >> mip, 1u);
	const uint32_t height = std::max((static_cast<uint32_t>(z.size.y_max) + 1u) >> mip, 1u);
	if (width > 16384 || height > 16384) {
		DepthFatal("invalid depth extent");
	}
	if (!has_depth && guest_samples != 1) {
		DepthFatal("unsupported multisampled stencil-only target: %u samples", guest_samples);
	}
	const auto* policy = has_depth ? FindDepthFormatPolicy(z.z_info.format) : nullptr;
	if (has_depth && policy == nullptr) {
		DepthFatal("unsupported depth/stencil format pair (z format %u, stencil %d)", static_cast<unsigned>(z.z_info.format), has_stencil ? 1 : 0);
	}
	const auto ideal_format =
	    policy != nullptr ? DepthAttachmentFormat(*policy, has_stencil) : vk::Format::eS8Uint;
	const auto format = ResolveHostDepthAttachmentFormat(buffer, policy, has_stencil, &samples);
	if (format == vk::Format::eUndefined) {
		DepthFatal("no host depth/stencil format supports required usage for %s",
		           vk::to_string(ideal_format).c_str());
	}
	if (samples != guest_samples) {
		Common::DebugFlags::NoteOnce("CapyPS5-Msaa", depth_address,
		    "depth target %ux%u: host does not support %u-sample depth; using %u",
		    width, height, guest_samples, samples);
	}
	const auto guest_format =
	    policy != nullptr ? policy->guest_format : Prospero::BufferFormat::k8UInt;
	const uint32_t bytes = policy != nullptr ? policy->bytes_per_element : 1;
	const auto     pitch        = TileGetDepthPitch(width, bytes, z.z_info.num_samples);
	TileSizeAlign depth_size {};
	TileSizeAlign stencil_size {};
	TileSizeAlign htile_size {};
	if (!TileGetDepthSize(width, height, 0, z.z_info.format, z.stencil_info.format, has_htile,
	                      stencil_size, htile_size, depth_size, z.z_info.num_samples) ||
	    (has_depth && (depth_size.align != 65536 || depth_size.size == 0)) ||
	    (has_stencil != (stencil_size.align == 65536 && stencil_size.size != 0)) ||
	    (has_htile != (htile_size.align == 32768 && htile_size.size != 0))) {
		DepthFatal("unsupported depth/stencil/HTile footprint");
	}
	if (depth_size.size > UINT64_MAX / view.image_layers ||
	    stencil_size.size > UINT64_MAX / view.image_layers ||
	    htile_size.size > UINT64_MAX / view.image_layers) {
		DepthFatal("layered depth footprint overflow");
	}
	const auto depth_backing_size   = depth_size.size * view.image_layers;
	const auto stencil_backing_size = stencil_size.size * view.image_layers;
	const auto htile_backing_size   = htile_size.size * view.image_layers;
	if ((has_depth && !GuestRange {depth_address, depth_backing_size}.Valid()) ||
	    (has_stencil && !GuestRange {stencil_address, stencil_backing_size}.Valid()) ||
	    (has_htile && !GuestRange {z.htile_data_base_addr, htile_backing_size}.Valid())) {
		DepthFatal("layered depth backing range is invalid");
	}
	// A stencil-only target is an S8 image over the stencil surface.
	const GuestRange        stencil_range {stencil_address, stencil_backing_size};
	TextureCache::ImageDesc desc {};
	desc.type         = TextureCache::BindingType::DepthTarget;
	desc.info.data    = has_depth ? GuestRange {depth_address, depth_backing_size} : stencil_range;
	desc.info.stencil = has_depth && has_stencil ? stencil_range : GuestRange {};
	desc.info.pixel_format    = format;
	desc.info.guest_format    = guest_format;
	desc.info.type            = Prospero::ImageType::kColor2D;
	desc.info.extent          = {width, height, 1};
	desc.info.resources       = {1, view.image_layers};
	desc.info.pitch           = pitch;
	desc.info.bytes_per_block = bytes;
	desc.info.samples         = samples;
	desc.info.tile_mode       = Prospero::TileMode::kDepth;
	desc.info.UpdateSize();
	desc.info.metadata.range =
	    has_htile ? GuestRange {z.htile_data_base_addr, htile_backing_size} : GuestRange {};
	desc.info.metadata.kind   = has_htile ? ImageMetadataKind::Htile : ImageMetadataKind::None;
	if (has_htile && gfx7_target && z.size.gfx7_pitch != 0 && z.size.gfx7_height != 0) {
		// The base console's 8-pipe P8_32x32_16x16 layout (KernelIsNeoMode answers 0).
		const bool     linear  = (z.gfx7_htile_surface & 1u) != 0;
		const uint32_t tiles_x = z.size.gfx7_pitch / 8u;
		const uint32_t tiles_y = z.size.gfx7_height / 8u;
		uint64_t       slice   = 0;
		if (linear) {
			// Rows of 4x4-tile groups, two elements apart, spread over 8 pipes of 256 bytes.
			const uint64_t per_pipe = (static_cast<uint64_t>(tiles_x / 4u) * ((tiles_y + 3u) / 4u) * 2u + 4u) * 4u;
			slice                   = Common::AlignUp(per_pipe, 256) * 8u;
		} else {
			// 64x64-tile macro tiles of 2 KiB per pipe.
			slice = static_cast<uint64_t>((tiles_x + 63u) / 64u) * ((tiles_y + 63u) / 64u) * 2048u * 8u;
		}
		desc.info.metadata.gfx7_htile         = static_cast<uint8_t>(1u + (linear ? 0u : 1u));
		desc.info.metadata.gfx7_htile_tiles_x = tiles_x;
		desc.info.metadata.gfx7_htile_slice   = slice;
	}
	desc.info.metadata.stencil_compressed =
	    has_stencil && has_htile && !z.stencil_info.htile_stencil_disabled;
	desc.view_info.format = format;
	desc.view_info.type =
	    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	desc.view_info.aspect      = ImageViewOps::DepthAspectMask(format);
	desc.view_info.base_level  = 0;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	return desc;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& r) {
	KYTY_PROFILER_FUNCTION();
	const auto& hw          = buffer.GetRegisters();
	const auto& z           = hw.GetDepthRenderTarget();
	const auto& rc          = hw.GetRenderControl();
	const auto& dc          = hw.GetDepthControl();
	const auto& sc          = hw.GetStencilControl();
	const auto& sm          = hw.GetStencilMask();
	const bool  has_depth   = z.z_info.format != Prospero::DepthFormat::kInvalid;
	const bool  has_stencil = z.stencil_info.format != Prospero::StencilFormat::kInvalid;
	// With HTile, DEPTH_COMPRESS_DISABLE expands touched tiles even with depth/stencil tests off.
	// PPSA32236 fast-clears HTile each frame, runs this pass, then samples the depth as raw memory.
	// Unbound, the clear never lands: NaN depth makes one compute pass ~95 ms (60 -> 10 fps).
	const bool depth_active =
	    has_depth &&
	    (dc.z_enable || dc.depth_bounds_enable || rc.depth_clear_enable || rc.copy_depth_to_color ||
	     (rc.depth_compress_disable && z.z_info.htile_acceleration));
	const bool stencil_active =
	    has_stencil && (dc.stencil_enable || rc.stencil_clear_enable || rc.copy_stencil_to_color);
	if (!depth_active && !stencil_active) {
		return;
	}
	const bool attachment_unbound =
	    z.z_info.format == Prospero::DepthFormat::kInvalid &&
	    z.stencil_info.format == Prospero::StencilFormat::kInvalid && z.z_info.num_samples == 0 &&
	    z.z_info.texture_compatibility == Prospero::TextureCompatiblePlaneCompression::kDisable &&
	    !z.z_info.expclear_enabled && !z.z_info.partially_resident && z.z_info.max_mip_level == 0 &&
	    z.stencil_info.texture_compatibility == Prospero::TextureCompatibleStencil::kDisable &&
	    !z.stencil_info.expclear_enabled && !z.stencil_info.partially_resident &&
	    z.depth_view.slice_start == 0 && z.depth_view.slice_max == 0 &&
	    z.depth_view.current_mip_level == 0 && !z.depth_view.depth_write_disable &&
	    !z.depth_view.stencil_write_disable && z.z_read_base_addr == 0 && z.z_write_base_addr == 0 &&
	    z.stencil_read_base_addr == 0 && z.stencil_write_base_addr == 0 &&
	    z.htile_data_base_addr == 0 &&
	    // DB_DEPTH_SIZE_XY is independent state and may remain programmed after the attachment
	    // formats and addresses are unbound. A zero encoding is the valid 1x1 value, so its
	    // presence alone must not manufacture a depth attachment.
	    !z.z_info.htile_acceleration && z.shading_rate_encoding == 0 && z.size.x_max == 0 &&
	    z.size.y_max == 0;
	// Both formats INVALID disable the DB surfaces whatever else stays programmed (a PS4 title
	// leaves sizes and addresses of an earlier target behind).
	const bool formats_invalid = z.z_info.format == Prospero::DepthFormat::kInvalid &&
	                             z.stencil_info.format == Prospero::StencilFormat::kInvalid;
	if (attachment_unbound || formats_invalid) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: ignoring enabled depth/stencil state without a bound attachment\n");
		}
		return;
	}
	if (rc.copy_stencil_to_color ||
	    dc.zfunc > static_cast<uint8_t>(vk::CompareOp::eAlways) ||
	    (has_depth && !z.depth_view.depth_write_disable &&
	     z.z_write_base_addr != z.z_read_base_addr) ||
	    (has_stencil && !z.depth_view.stencil_write_disable &&
	     z.stencil_write_base_addr != z.stencil_read_base_addr)) {
		DepthFatal("unsupported depth state: stencil-to-color=%d, zfunc=%u, read=0x%" PRIx64 ", write=0x%" PRIx64
		           ", depth write disabled=%d",
		           rc.copy_stencil_to_color ? 1 : 0, static_cast<unsigned>(dc.zfunc), z.z_read_base_addr,
		           z.z_write_base_addr, z.depth_view.depth_write_disable ? 1 : 0);
	}
	if (rc.copy_depth_to_color &&
	    (z.z_info.num_samples != 0 || rc.copy_sample != 0 ||
	     z.z_info.format != Prospero::DepthFormat::kZ32F ||
	     dc.z_enable || dc.z_write_enable || dc.depth_bounds_enable || dc.stencil_enable ||
	     rc.depth_clear_enable || rc.stencil_clear_enable)) {
		DepthFatal("unsupported depth-to-color copy state");
	}
	// The target depends only on the depth registers (debug.capy.resolve_cache).
	const bool reused = m_context.GetResolveCache().FindDepth(z, r.desc, r.image_id);
	if (!reused) {
		r.desc = MakeDepthTargetDesc(buffer, z);
	}
	r.depth_clear_enable      = has_depth && rc.depth_clear_enable;
	r.depth_load_clear_enable = r.depth_clear_enable;
	r.depth_clear_value       = hw.GetDepthClearValue();
	r.depth_test_enable       = has_depth && dc.z_enable;
	r.depth_write_enable      = r.depth_test_enable && dc.z_write_enable &&
	                            !z.depth_view.depth_write_disable && !r.depth_clear_enable;
	r.depth_compare_op        = static_cast<vk::CompareOp>(dc.zfunc);
	// `setprop debug.capy.eq_depth N` (diagnosis): N=1 tests EQUAL as GREATER_OR_EQUAL, 2 as
	// LESS_OR_EQUAL, 3 as ALWAYS. Tells a depth-EQUAL pass that misses its prepass apart.
	static Common::DebugFlags::CachedValue eq_depth("debug.capy.eq_depth", 0);
	if (eq_depth.Get() != 0 && r.depth_compare_op == vk::CompareOp::eEqual) {
		r.depth_compare_op = eq_depth.Get() == 1   ? vk::CompareOp::eGreaterOrEqual
		                     : eq_depth.Get() == 2 ? vk::CompareOp::eLessOrEqual
		                                           : vk::CompareOp::eAlways;
	}

	r.depth_bounds_test_enable = has_depth && dc.depth_bounds_enable;

	r.stencil_clear_enable =
	    has_stencil && rc.stencil_clear_enable && !z.depth_view.stencil_write_disable;
	r.stencil_clear_value = hw.GetStencilClearValue();
	r.stencil_test_enable = has_stencil && dc.stencil_enable;
	if (r.stencil_test_enable) {
		const bool stencil_ops_disabled =
		    rc.stencil_clear_enable || z.depth_view.stencil_write_disable;
		const uint8_t front_write_mask = stencil_ops_disabled ? 0 : sm.stencil_writemask;
		const uint8_t back_write_mask  = stencil_ops_disabled ? 0 : sm.stencil_writemask_bf;
		if (dc.stencilfunc > static_cast<uint8_t>(vk::CompareOp::eAlways) ||
		    (dc.backface_enable &&
		     dc.stencilfunc_bf > static_cast<uint8_t>(vk::CompareOp::eAlways))) {
			DepthFatal("unsupported stencil compare state");
		}
		r.stencil_front = ConvertStencilState(
		    dc.stencilfunc, {sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail},
		    sm.stencil_opval, {.compareMask = sm.stencil_mask, .writeMask = front_write_mask,
		                       .reference = sm.stencil_testval});
		if (dc.backface_enable) {
			r.stencil_back = ConvertStencilState(
			    dc.stencilfunc_bf, {sc.stencil_fail_bf, sc.stencil_zpass_bf, sc.stencil_zfail_bf},
			    sm.stencil_opval_bf, {.compareMask = sm.stencil_mask_bf, .writeMask = back_write_mask,
			                          .reference = sm.stencil_testval_bf});
		} else {
			r.stencil_back = r.stencil_front;
		}
	}
	if (!reused) {
		auto& cache = m_context.GetTextureCache();
		r.image_id  = cache.FindImage(r.desc);
		m_context.GetResolveCache().SetDepth(z, r.desc, r.image_id);
	}
	BindRenderTarget(r.image_id);
}

bool RenderExecutor::DepthStencilCopy(CommandBuffer& buffer) {
	const auto& hw       = buffer.GetRegisters();
	const auto& z        = hw.GetDepthRenderTarget();
	const auto& override = hw.GetDepthRenderOverride();
	if (hw.GetColorControl().mode != 0) {
		return false;
	}
	const bool depth_copy = override.force_z_dirty && override.force_z_valid &&
	                        z.z_info.format != Prospero::DepthFormat::kInvalid &&
	                        z.z_read_base_addr != 0 && z.z_write_base_addr != 0 &&
	                        z.z_read_base_addr != z.z_write_base_addr;
	const bool stencil_copy = override.force_stencil_dirty && override.force_stencil_valid &&
	                          z.stencil_info.format != Prospero::StencilFormat::kInvalid &&
	                          z.stencil_read_base_addr != 0 && z.stencil_write_base_addr != 0 &&
	                          z.stencil_read_base_addr != z.stencil_write_base_addr;
	if (!depth_copy && !stencil_copy) {
		return false;
	}

	auto  read_desc  = MakeDepthTargetDesc(buffer, z);
	auto  write_desc = MakeDepthTargetDesc(buffer, z, true);
	auto& cache      = m_context.GetTextureCache();
	const auto read_id = cache.FindImage(read_desc);
	BindRenderTarget(read_id);
	const auto write_id = cache.FindImage(write_desc);
	BindRenderTarget(write_id);
	cache.UpdateImage(read_id);
	cache.UpdateImage(write_id);
	cache.MarkGpuWritten(write_id);
	auto& source      = cache.GetImage(read_id);
	auto& destination = cache.GetImage(write_id);
	EXIT_IF(read_id == write_id || source.backing.format != destination.backing.format);

	auto& scheduler = m_context.GetCommandScheduler();
	scheduler.EndRendering();
	const auto command = scheduler.Current().Handle();
	const ImageSubresourceRange range {read_desc.view_info.base_level, 1,
	                                  read_desc.view_info.base_layer,
	                                  read_desc.view_info.layer_count};
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	               range, command);
	destination.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	                    range, command);
	std::array<vk::ImageCopy, 2> regions {};
	uint32_t                   count = 0;
	for (const auto aspect: {vk::ImageAspectFlagBits::eDepth, vk::ImageAspectFlagBits::eStencil}) {
		if ((aspect == vk::ImageAspectFlagBits::eDepth && !depth_copy) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil && !stencil_copy)) {
			continue;
		}
		auto& region = regions[count++];
		region.srcSubresource = {aspect, range.base_level, range.base_layer,
		                         range.layer_count};
		region.dstSubresource = region.srcSubresource;
		region.extent = write_desc.info.extent;
	}
	Common::DebugFlags::LogDepthWrite("DepthCopy", destination.track_addr);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
	                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
	                  count, regions.data());
	return true;
}

vk::ImageAspectFlags RenderDepthInfo::AttachmentWriteAspects() const {
	const auto format = desc.view_info.format;
	if (format == vk::Format::eUndefined) {
		return {};
	}

	const auto           available = ImageViewOps::DepthAspectMask(format);
	vk::ImageAspectFlags writes {};
	if ((available & vk::ImageAspectFlagBits::eDepth) &&
	    (depth_load_clear_enable || depth_write_enable)) {
		writes |= vk::ImageAspectFlagBits::eDepth;
	}
	if (!(available & vk::ImageAspectFlagBits::eStencil)) {
		return writes;
	}

	const auto face_writes = [&](const vk::StencilOpState& state) {
		if (state.writeMask == 0) {
			return false;
		}
		bool can_pass = state.compareOp != vk::CompareOp::eNever;
		bool can_fail = state.compareOp != vk::CompareOp::eAlways;
		if (state.compareMask == 0) {
			switch (state.compareOp) {
				case vk::CompareOp::eEqual:
				case vk::CompareOp::eLessOrEqual:
				case vk::CompareOp::eGreaterOrEqual:
				case vk::CompareOp::eAlways:
					can_pass = true;
					can_fail = false;
					break;
				case vk::CompareOp::eNever:
				case vk::CompareOp::eLess:
				case vk::CompareOp::eGreater:
				case vk::CompareOp::eNotEqual:
					can_pass = false;
					can_fail = true;
					break;
				default: break;
			}
		}
		const bool depth_pass = !depth_test_enable || depth_compare_op != vk::CompareOp::eNever;
		const bool depth_fail = depth_test_enable && depth_compare_op != vk::CompareOp::eAlways;
		return (can_fail && state.failOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_pass && state.passOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_fail && state.depthFailOp != vk::StencilOp::eKeep);
	};
	if (stencil_clear_enable ||
	    (stencil_test_enable && (face_writes(stencil_front) || face_writes(stencil_back)))) {
		writes |= vk::ImageAspectFlagBits::eStencil;
	}
	return writes;
}

} // namespace Libs::Graphics
