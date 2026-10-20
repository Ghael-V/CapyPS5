// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "common/debugFlags.h"
#if defined(__ANDROID__)
#include "android/capyps5_jni.h"
#endif
#include "common/file.h"
#include "common/passInspector.h"
#include "common/telemetry.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.guest_width, rendering.guest_height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	// Without multiViewport only viewport 0 exists; a shader-selected index then lands there.
	const uint32_t viewport_count =
	    indexed_viewports && buffer.GetGraphics().multi_viewport_enabled ? viewport_slots : 1;
	// PA_SU_VTX_CNTL.PIX_CENTER = 0 puts pixel centers at integer coordinates (D3D9 style), while
	// Vulkan samples at +0.5. Moving the geometry half a pixel right/down makes Vulkan sample the
	// same spot. `debug.capy.pix_center`: 0 = follow the register, 1 = never shift, 2 = always.
	static Common::DebugFlags::CachedValue pix_center_mode("debug.capy.pix_center", 0);
	const int64_t pix_center = pix_center_mode.Get();
	const float   pixel_shift =
	    (pix_center == 2 || (pix_center == 0 && (ctx.GetVtxControl() & 1u) == 0)) ? 0.5f : 0.0f;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
			viewport.x      = pixel_shift;
			viewport.y      = pixel_shift;
		} else {
			viewport.x      = guest.xoffset - guest.xscale + pixel_shift;
			viewport.y      = guest.yoffset - guest.yscale + pixel_shift;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;
		// Diagnose guest ranges outside Vulkan's standard [0, 1] viewport depth range.
		// This changes the depth transform and stays off until validated for the guest.
		static Common::DebugFlags::CachedValue clamp_depth_range("debug.capy.clamp_depth_range", 0);
		if (clamp_depth_range.Get() != 0) {
			viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
			viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
		}

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	if (rendering.scale != 1.0f) {
		// Downscaled attachments (debug.capy.res_scale): map guest pixels onto host pixels.
		const float scale = rendering.scale;
		for (uint32_t i = 0; i < viewport_count; i++) {
			viewports[i].x *= scale;
			viewports[i].y *= scale;
			viewports[i].width *= scale;
			viewports[i].height *= scale;
			const auto x0 = static_cast<int32_t>(static_cast<float>(scissors[i].offset.x) * scale);
			const auto y0 = static_cast<int32_t>(static_cast<float>(scissors[i].offset.y) * scale);
			const auto x1 = static_cast<int32_t>(std::ceil(
			    static_cast<float>(scissors[i].offset.x + static_cast<int32_t>(scissors[i].extent.width)) *
			    scale));
			const auto y1 = static_cast<int32_t>(std::ceil(
			    static_cast<float>(scissors[i].offset.y + static_cast<int32_t>(scissors[i].extent.height)) *
			    scale));
			scissors[i].offset = vk::Offset2D {x0, y0};
			scissors[i].extent = vk::Extent2D {
			    static_cast<uint32_t>(std::min<int32_t>(x1, static_cast<int32_t>(rendering.width)) -
			                          std::min(x0, static_cast<int32_t>(rendering.width))),
			    static_cast<uint32_t>(std::min<int32_t>(y1, static_cast<int32_t>(rendering.height)) -
			                          std::min(y0, static_cast<int32_t>(rendering.height)))};
		}
	}
	if (Common::DebugFlags::FrameTracing()) {
		const auto& g = vp.viewports[0];
		Common::DebugFlags::FrameTrace(
		    "    viewport x=%.2f y=%.2f w=%.2f h=%.2f z=[%.4f,%.4f] guest zscale=%.4f zoffset=%.4f "
		    "dx=%d clipdis=%d vtx_cntl=0x%x",
		    viewports[0].x, viewports[0].y, viewports[0].width, viewports[0].height,
		    viewports[0].minDepth, viewports[0].maxDepth, g.zscale, g.zoffset,
		    ctx.GetClipControl().dx_clip_space ? 1 : 0, ctx.GetClipControl().clip_disable ? 1 : 0,
		    ctx.GetVtxControl());
	}
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	// Bisection aid: `setprop debug.capy.no_ro_depth 1` disables depth testing for draws that
	// test without writing (Adreno LRZ investigation).
	static Common::DebugFlags::CachedValue no_ro_depth("debug.capy.no_ro_depth", 0);
	static Common::DebugFlags::CachedValue no_depth("debug.capy.no_depth", 0);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable && no_depth.Get() == 0 &&
	                                   (depth.depth_write_enable || no_ro_depth.Get() == 0));
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable && no_depth.Get() == 0);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
	if (buffer.GetGraphics().depth_bounds_enabled) {
		dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
		if (depth.depth_bounds_test_enable) {
			dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
		}
	}

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}
	if (Common::DebugFlags::FrameTracing()) {
		Common::DebugFlags::FrameTrace(
		    "    zstate test=%d write=%d op=%d bounds=%d [%.4f,%.4f] bias=%d front=%d back=%d "
		    "fo=%.4f fs=%.4f bo=%.4f bs=%.4f clamp=%.4f",
		    depth.depth_test_enable ? 1 : 0, depth.depth_write_enable ? 1 : 0,
		    static_cast<int>(depth.depth_compare_op), depth.depth_bounds_test_enable ? 1 : 0,
		    ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax(), depth_bias_enable ? 1 : 0,
		    mode.poly_offset_front_enable ? 1 : 0, mode.poly_offset_back_enable ? 1 : 0,
		    poly_offset.front_offset, poly_offset.front_scale, poly_offset.back_offset,
		    poly_offset.back_scale, poly_offset.clamp);
	}

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}

	// Without VK_EXT_color_write_enable (MoltenVK, Mali) the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
	if (buffer.GetGraphics().color_write_enable_enabled) {
		vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
		for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
			enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
		}
		dynamic_state.SetColorWriteEnables(std::span {enable}.first(rendering.num_color_attachments));
	}
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop || sh_regs.AlphaToMask();
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
	uint32_t             indirect_vertex_sgpr   = UINT32_MAX;
	uint32_t             indirect_instance_sgpr = UINT32_MAX;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.guest_width           = std::numeric_limits<uint32_t>::max();
	state.guest_height          = std::numeric_limits<uint32_t>::max();
	bool scale_known            = false;
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		if (layout == vk::ImageLayout::eGeneral && image.backing.extent.width >= 1920) {
			// GENERAL may cost the Adreno its framebuffer compression on large targets.
			Common::DebugFlags::NoteOnce("CapyPS5-Layout", image.info.data.address ^ 0x47454e4552414cull,
			                             "color target 0x%llx %ux%u fmt=%u bound as a texture too: GENERAL layout",
			                             static_cast<unsigned long long>(image.info.data.address),
			                             image.backing.extent.width, image.backing.extent.height,
			                             static_cast<uint32_t>(image.backing.format));
		}
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		const auto host_level   = view.base_level;
		state.width  = std::min(state.width, std::max(image.backing.extent.width >> host_level, 1u));
		state.height = std::min(state.height, std::max(image.backing.extent.height >> host_level, 1u));
		state.guest_width  = std::min(state.guest_width, extent.width);
		state.guest_height = std::min(state.guest_height, extent.height);
		if (scale_known && image.scale != state.scale) {
			Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x534d495845ull,
			                             "render pass mixes differently scaled attachments");
		}
		state.scale = std::min(state.scale, image.scale);
		scale_known = true;
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// Keep the layout the attachment already has when it also serves this draw: Stray
		// alternates stencil-writing and read-only draws on its 4K depth, and switching between
		// the two read-only layouts ended the pass at every draw.
		// `setprop debug.capy.keep_ds_layout 0` picks the layout per draw as before.
		const auto is_attachment_layout = [](vk::ImageLayout candidate) {
			return DepthWritableAspects(candidate) || candidate == vk::ImageLayout::eDepthStencilReadOnlyOptimal ||
			       candidate == vk::ImageLayout::eDepthReadOnlyOptimal ||
			       candidate == vk::ImageLayout::eStencilReadOnlyOptimal;
		};
		static Common::DebugFlags::CachedValue keep_ds_layout("debug.capy.keep_ds_layout", 1);
		const auto current_layout = image.backing.state.layout;
		if (keep_ds_layout.Get() != 0 && current_layout != layout && image.backing.subresource_states.empty() &&
		    is_attachment_layout(current_layout) && !(draw_writes & ~DepthWritableAspects(current_layout)) &&
		    !(sampled_aspects & ~DepthReadableAspects(current_layout)) &&
		    !(DepthReadableAspects(layout) & ~DepthReadableAspects(current_layout) & sampled_aspects)) {
			layout = current_layout;
		}
		// When neither layout serves both draws (God of War Sons of Sparta and Stray alternate a
		// draw that samples the stencil with one that writes it), switching ended the pass at every
		// draw. The feedback-loop layout allows both; draws that sample the attachment then enable
		// the feedback loop for those aspects. `setprop debug.capy.ds_feedback_merge 0` disables it.
		static Common::DebugFlags::CachedValue ds_feedback_merge("debug.capy.ds_feedback_merge", 1);
		if (ds_feedback_merge.Get() != 0 && m_context.GetGraphics().attachment_feedback_loop_enabled &&
		    current_layout != layout && image.backing.subresource_states.empty() &&
		    is_attachment_layout(current_layout) &&
		    is_attachment_layout(layout) && buffer.IsRendering()) {
			layout = vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT;
		}
		if (layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT) {
			feedback_aspects |= sampled_aspects;
		}
#if defined(__ANDROID__)
		// `setprop debug.capy.ds_log N`: log the next N depth layout decisions that change layout.
		static Common::DebugFlags::CachedValue ds_log("debug.capy.ds_log", 0);
		static int64_t                         ds_armed = 0;
		static int64_t                         ds_left  = 0;
		if (ds_log.Get() != ds_armed) {
			ds_armed = ds_log.Get();
			ds_left  = ds_armed;
		}
		if (ds_left > 0 && current_layout != layout) {
			ds_left--;
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-DsLayout",
			                    "0x%llx cur=%u new=%u bound=%u writes=0x%x sampled=0x%x sub=%zu rendering=%d fbl=%d",
			                    static_cast<unsigned long long>(depth.desc.info.data.address),
			                    static_cast<uint32_t>(current_layout), static_cast<uint32_t>(layout),
			                    static_cast<uint32_t>(image.binding.attachment_layout),
			                    static_cast<uint32_t>(draw_writes), static_cast<uint32_t>(sampled_aspects),
			                    image.backing.subresource_states.size(), buffer.IsRendering() ? 1 : 0,
			                    m_context.GetGraphics().attachment_feedback_loop_enabled ? 1 : 0);
		}
#endif
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width        = std::min(state.width, image.backing.extent.width);
		state.height       = std::min(state.height, image.backing.extent.height);
		state.guest_width  = std::min(state.guest_width, depth.desc.info.extent.width);
		state.guest_height = std::min(state.guest_height, depth.desc.info.extent.height);
		if (scale_known && image.scale != state.scale) {
			Common::DebugFlags::NoteOnce("CapyPS5-Scale", 0x534d495845ull,
			                             "render pass mixes differently scaled attachments");
		}
		// The color targets set the scale when there are any: The Binding of Isaac draws its room
		// into unscaled 512x320 targets with the scaled main depth buffer bound, and taking the
		// depth scale shrank the viewport to the top-left of the room.
		if (!scale_known) {
			state.scale = image.scale;
		}
		scale_known = true;
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
		state.guest_width  = state.width;
		state.guest_height = state.height;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh_regs.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh_regs.target_output_mode[index] != 0 &&
		    render_target_mask_slot(write_mask, slot) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted. Tracked DCC clear state is materialized on attachment bind;
	// future CMask/FMask support can consume its state through the same TextureCache path.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
	// GPU-side indirect draw arguments in guest memory (DrawIndexArgs::indirect_args).
	uint64_t indirect_args   = 0;
	uint32_t indirect_count  = 0;
	uint32_t indirect_stride = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		if (size == 0) {
			Common::DebugFlags::NoteOnce("CapyPS5-Vtx", range.base_address >> 12u,
			    "vertex range 0x%llx size=%llu is outside guest memory: drawing with a null buffer",
			    static_cast<unsigned long long>(range.base_address),
			    static_cast<unsigned long long>(range.RequestedSize()));
			range.acquired_end = range.base_address;
			range.binding      = {&cache.GetBuffer(NULL_BUFFER_ID), 0};
			continue;
		}
		range.acquired_end = range.base_address + size;
		if (Common::DebugFlags::FrameTracing()) {
			uint32_t words[8] = {};
			(void)Libs::LibKernel::Memory::ReadGuestMemory(range.base_address, words,
			                                               std::min<uint64_t>(size, sizeof(words)));
			Common::DebugFlags::FrameTrace("    vb 0x%llx size=%llu %08x %08x %08x %08x %08x %08x %08x %08x",
			                               static_cast<unsigned long long>(range.base_address),
			                               static_cast<unsigned long long>(size), words[0], words[1],
			                               words[2], words[3], words[4], words[5], words[6], words[7]);
			// `debug.capy.vb_full 1`: dump whole vertex ranges (up to 8 KiB), 16 dwords per line.
			static Common::DebugFlags::CachedValue vb_full("debug.capy.vb_full", 0);
			if (vb_full.Get() != 0 && size <= 8192) {
				std::vector<uint32_t> all((size + 3) / 4);
				(void)Libs::LibKernel::Memory::ReadGuestMemory(range.base_address, all.data(), size);
				for (size_t w = 0; w < all.size(); w += 16) {
					std::string line;
					for (size_t k = w; k < std::min(all.size(), w + 16); k++) {
						char hex[12];
						std::snprintf(hex, sizeof(hex), " %08x", all[k]);
						line += hex;
					}
					Common::DebugFlags::FrameTrace("    vbd 0x%llx%s",
					    static_cast<unsigned long long>(range.base_address + w * 4), line.c_str());
				}
			}
		}
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count || range->binding.first == nullptr) {
			Common::DebugFlags::NoteOnce("CapyPS5-Vtx", vertex.addr >> 12u ^ 0x8000000000ull,
			    "vertex buffer 0x%llx has no acquired range: bound to a null buffer",
			    static_cast<unsigned long long>(vertex.addr));
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			prepared.sizes[i]   = 0;
			continue;
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	std::vector<uint8_t> buffer_data(source.size);
	if (!Libs::LibKernel::Memory::ReadGuestMemory(source.address, buffer_data.data(), source.size)) {
		return false;
	}
	const auto* indices = buffer_data.data();
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info,
	    draw.indirect_vertex_sgpr, draw.indirect_instance_sgpr);
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	Common::DebugFlags::FrameTraceDrawTick();
	// Merged ES+GS (geometry shader) draws: mesh shaders, or compute emulation without them.
	const bool gs_emulated = (buffer.GetRegisters().GetShaderStages() & 0x20u) != 0 &&
	                     !m_context.GetGraphics().mesh_shader_enabled;
	{
		// `debug.capy.gpu_stats 1`: how many draws run ES+GS through compute out of the total.
		static Common::DebugFlags::CachedValue gpu_stats("debug.capy.gpu_stats", 0);
		static uint64_t total = 0, emulated = 0;
		static auto     next = std::chrono::steady_clock::now();
		if (gpu_stats.Get() != 0) {
			total++;
			emulated += gs_emulated ? 1 : 0;
			if (const auto now = std::chrono::steady_clock::now(); now >= next) {
				next = now + std::chrono::seconds(2);
#if defined(__ANDROID__)
				static uint64_t last_submits = 0;
				const auto      submits      = g_submit_count.load(std::memory_order_relaxed);
				__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuStats", "draws %llu, submits %llu, ES+GS emulated %llu (stages 0x%x)",
				                    static_cast<unsigned long long>(total),
				                    static_cast<unsigned long long>(submits - last_submits),
				                    static_cast<unsigned long long>(emulated),
				                    static_cast<unsigned>(buffer.GetRegisters().GetShaderStages()));
				last_submits = submits;
#endif
				total = emulated = 0;
			}
		}
	}
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt) {
				mrt_mask |= 1u << output.location;
			}
		}
	}
	mrt_mask &= color_output_mask;
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);
	// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
	// keep those matching the raster count instead of resolving multisampled shadows.
	const auto colors       = std::span {state.color_info, state.color_count};
	const auto pass_samples = state.depth_info.image_id ? state.depth_info.desc.info.samples
	                                                    : state.color_info[0].desc.info.samples;
	if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
		    return color.desc.info.samples != pass_samples;
	    })) {
		const auto raster =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
		const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
			return color.desc.info.samples != raster;
		});
		state.color_count -= static_cast<uint32_t>(dropped.size());
		if (state.depth_info.image_id && state.depth_info.desc.info.samples != raster) {
			state.depth_info = {};
		}
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: dropping attachments whose sample count differs from the raster.\n");
		}
	}

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		Common::DebugFlags::FrameTrace("    ib 0x%llx size=%llu",
		                               static_cast<unsigned long long>(source.address),
		                               static_cast<unsigned long long>(source.size));
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		if (prepared.buffers[i] == nullptr) {
			Common::DebugFlags::NoteOnce("CapyPS5-Vtx", 0xdead0000ull + i,
			    "vertex binding %u is null: vertex buffers not bound for this draw", i);
			return;
		}
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit,
                               vk::Buffer indirect_buffer = nullptr,
                               vk::DeviceSize indirect_offset = 0) {
	if (indirect_buffer != nullptr) {
		// One command per draw: a stride above the argument size needs no multiDrawIndirect.
		for (uint32_t i = 0; i < emit.indirect_count; i++) {
			const auto offset = indirect_offset + static_cast<vk::DeviceSize>(i) * emit.indirect_stride;
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexedIndirect(indirect_buffer, offset, 1, 0);
			} else {
				vk_buffer.drawIndirect(indirect_buffer, offset, 1, 0);
			}
		}
		return;
	}
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                       const DrawEmitInfo& emit, DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto& depth = state.depth_info;
	auto& color = state.color_info[0];
	EXIT_NOT_IMPLEMENTED(!m_context.GetGraphics().depth_color_copy_enabled ||
	                     !Prospero::IsRectList(ucfg.GetPrimType()) ||
	                     draw.IsIndexed() || draw.index_count != 3 || draw.instance_count != 1 ||
	                     emit.first_vertex != 0 || emit.first_instance != 0 ||
	                     state.color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	                     color.desc.info.samples != 1 ||
	                     depth.desc.info.extent.width != color.Extent().width ||
	                     depth.desc.info.extent.height != color.Extent().height ||
	                     depth.desc.view_info.layer_count != 1 ||
	                     color.desc.view_info.layer_count != 1 ||
	                     hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	                     state.ps_input_info.ps_depth_export_enable ||
	                     state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.depth_load_clear_enable) {
		m_context.GetCommandScheduler().BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	auto rect = calc_final_scissor(hw.GetScreenViewport(), hw.GetScanModeControl(),
	                               {rendering.guest_width, rendering.guest_height}, 0);
	if (rendering.scale != 1.0f) {
		const auto host = [&](int32_t value, uint32_t limit) {
			return std::min(static_cast<int32_t>(std::lround(static_cast<float>(value) * rendering.scale)),
			                static_cast<int32_t>(limit));
		};
		rect.left   = host(rect.left, rendering.width);
		rect.right  = host(rect.right, rendering.width);
		rect.top    = host(rect.top, rendering.height);
		rect.bottom = host(rect.bottom, rendering.height);
	}
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		m_context.GetCommandScheduler().EndRendering();
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	auto& ucfg = buffer.GetUserConfig();
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	// Bisection aid: `setprop debug.capy.skip_ps 0x<address>` drops draws using that pixel shader.
	static Common::DebugFlags::CachedValue skip_ps("debug.capy.skip_ps", 0);
	const auto pixel_shader = buffer.GetShaders().GetPs().ps_regs.data_addr;
	if (pixel_shader != 0) {
		const auto extent = state.color_count != 0 ? state.color_info[0].Extent() : vk::Extent2D {};
		Common::PassInspector::RecordDraw(pixel_shader, extent.width, extent.height,
		                                  state.color_count);
	}
	// `setprop debug.capy.skip_nth N` (1-based) narrows skip_ps to the Nth draw of that shader per frame.
	static Common::DebugFlags::CachedValue skip_nth("debug.capy.skip_nth", 0);
	static uint64_t last_frame = 0;
	static int64_t  nth_seen   = 0;
	if (const auto frame = Common::Telemetry::g_game_frames.load(std::memory_order_relaxed);
	    frame != last_frame) {
		last_frame = frame;
		nth_seen   = 0;
	}
	bool skip_this = true;
	if (skip_ps.Get() != 0 && static_cast<uint64_t>(skip_ps.Get()) == pixel_shader &&
	    skip_nth.Get() != 0) {
		skip_this = ++nth_seen == skip_nth.Get();
	}
	if (const auto skip = skip_ps.Get();
	    (skip != 0 && static_cast<uint64_t>(skip) == pixel_shader && skip_this) ||
	    Common::PassInspector::IsSkipped(pixel_shader)) {
		return;
	}
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	const bool mesh_emulated = mesh_active && state.vertex_info[0].stage.program->mesh_compute;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		const auto& compute_limits = m_context.GetGraphics().GetPhysicalDeviceProperties().limits;
		if (mesh_emulated) {
			if (mesh_groups > compute_limits.maxComputeWorkGroupCount[0] ||
			    draw.instance_count > compute_limits.maxComputeWorkGroupCount[1]) {
				EXIT("emulated mesh draw exceeds compute workgroup limits: %ux%u\n", mesh_groups,
				     draw.instance_count);
			}
		} else if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	// Shader buffer writes earlier in this pass are only made visible when the pass ends (see the
	// deferred write barrier below). If this draw reads one of those ranges, end the pass first.
	{
		auto& scheduler = m_context.GetCommandScheduler();
		if (m_pass_writes_serial != CommandBuffer::PassSerial() || !scheduler.Current().IsRendering()) {
			m_pass_writes.clear();
			m_pass_writes_serial = CommandBuffer::PassSerial();
		}
		if (!m_pass_writes.empty()) {
			const auto overlaps = [&](uint64_t address, uint64_t size) {
				return size != 0 && std::ranges::any_of(m_pass_writes, [&](const auto& written) {
					return address < written.first + written.second && written.first < address + size;
				});
			};
			bool hazard = overlaps(index_source.address,
			                       static_cast<uint64_t>(draw.index_count) * index_source.guest_element_size);
			for (const auto* stage: stages) {
				for (const auto& source: stage->buffer_sources) {
					hazard |= overlaps(source.address, source.size);
				}
			}
			if (hazard) {
				scheduler.EndRendering();
				m_pass_writes.clear();
			}
		}
	}
	// `setprop debug.capy.skip_tex 0x<address>` drops pixel-shader draws that sample that texture.
	static Common::DebugFlags::CachedValue skip_tex("debug.capy.skip_tex", 0);
	if (const auto tex = skip_tex.Get(); tex != 0 && state.ps_active) {
		for (const auto& image: bindings.pixel->images) {
			if (image.desc.info.data.address == static_cast<uint64_t>(tex)) {
				return;
			}
		}
	}
	// `setprop debug.capy.dump_ps 0x<ps>`: during a frame trace, write every image this pixel
	// shader samples back to guest memory so it can be read from /proc/<pid>/mem.
	static Common::DebugFlags::CachedValue dump_ps("debug.capy.dump_ps", 0);
	if (Common::DebugFlags::FrameTracing() && state.ps_active && dump_ps.Get() != 0 &&
	    static_cast<uint64_t>(dump_ps.Get()) == buffer.GetShaders().GetPs().ps_regs.data_addr) {
		{
			ShaderRecompiler::Decoder::Program program;
			const auto  ps_addr = buffer.GetShaders().GetPs().ps_regs.data_addr;
			const auto* code    = reinterpret_cast<const uint32_t*>(ps_addr);
			const auto  words   = ShaderCodeWords(ps_addr);
			if (words != 0) {
				ShaderRecompiler::Decoder::DecodeProgram(std::span<const uint32_t>(code, words), program);
			}
			const auto text = ShaderRecompiler::Decoder::ProgramToString(program);
			size_t start = 0;
			while (start < text.size()) {
				const auto end = text.find(static_cast<char>(10), start);
				const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				Common::DebugFlags::FrameTrace("  ps| %s", line.c_str());
				if (end == std::string::npos) break;
				start = end + 1;
			}
		}
		// The vertex stage too (es or gs registers, whichever is set).
		for (const uint64_t vs_addr: {buffer.GetShaders().GetVs().es_regs.data_addr,
		                              buffer.GetShaders().GetVs().gs_regs.data_addr}) {
			if (vs_addr == 0) {
				continue;
			}
			ShaderRecompiler::Decoder::Program program;
			const auto* code  = reinterpret_cast<const uint32_t*>(vs_addr);
			const auto  words = ShaderCodeWords(vs_addr);
			if (words == 0) {
				continue;
			}
			ShaderRecompiler::Decoder::DecodeProgram(std::span<const uint32_t>(code, words), program);
			const auto text = ShaderRecompiler::Decoder::ProgramToString(program);
			Common::DebugFlags::FrameTrace("  vs 0x%llx", static_cast<unsigned long long>(vs_addr));
			size_t start = 0;
			while (start < text.size()) {
				const auto end  = text.find(static_cast<char>(10), start);
				const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				Common::DebugFlags::FrameTrace("  vs| %s", line.c_str());
				if (end == std::string::npos) break;
				start = end + 1;
			}
		}
		if (state.depth_info.image_id) {
			const bool ok = m_context.GetTextureCache().DebugDownloadImage(state.depth_info.image_id);
			Common::DebugFlags::FrameTrace("  dump depth addr=0x%llx -> %d",
			                               static_cast<unsigned long long>(state.depth_info.desc.info.data.address),
			                               ok ? 1 : 0);
		}
		for (const auto& image: bindings.pixel->images) {
			const bool ok = m_context.GetTextureCache().DebugDownloadImage(image.image_id);
			Common::DebugFlags::FrameTrace("  dump image addr=0x%llx -> %d",
			                               static_cast<unsigned long long>(image.desc.info.data.address),
			                               ok ? 1 : 0);
		}
	}
	// `setprop debug.capy.watch_ps 0x<ps>`: one line per draw of that pixel shader (no frame trace)
	// with its user data, the memory its SRT pointer s[28:29] reaches and its images, so frames
	// that render wrong can be told apart. Tag CapyPS5-Watch.
	static Common::DebugFlags::CachedValue watch_ps("debug.capy.watch_ps", 0);
	if (state.ps_active && watch_ps.Get() != 0 &&
	    static_cast<uint64_t>(watch_ps.Get()) == buffer.GetShaders().GetPs().ps_regs.data_addr) {
		const auto& sgpr = buffer.GetShaders().GetPs().ps_user_sgpr;
		auto hash_mem = [](uint64_t addr, uint64_t size) {
			std::vector<uint32_t> words(size / 4);
			if (addr == 0 || !Libs::LibKernel::Memory::ReadGuestMemory(addr, words.data(), size)) {
				return uint64_t {0};
			}
			uint64_t h = 1469598103934665603ull;
			for (const auto w: words) {
				h = (h ^ w) * 1099511628211ull;
			}
			return h;
		};
		const uint64_t srt = sgpr.value[28] | (static_cast<uint64_t>(sgpr.value[29] & 0xffffu) << 32u);
		uint32_t vsharp[12] = {};
		if (srt != 0) {
			(void)Libs::LibKernel::Memory::ReadGuestMemory(srt, vsharp, sizeof(vsharp));
		}
		std::string line;
		char        tmp[96];
		for (int v = 0; v < 3; v++) {
			const uint64_t base = vsharp[v * 4] | (static_cast<uint64_t>(vsharp[v * 4 + 1] & 0xffffu) << 32u);
			std::snprintf(tmp, sizeof(tmp), " cb%d=0x%llx:%016llx", v, static_cast<unsigned long long>(base),
			              static_cast<unsigned long long>(hash_mem(base, 128)));
			line += tmp;
		}
		for (const auto& image: bindings.pixel->images) {
			std::snprintf(tmp, sizeof(tmp), " img%u@0x%llx", static_cast<uint32_t>(image.image_id.index),
			              static_cast<unsigned long long>(image.desc.info.data.address));
			line += tmp;
		}
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Watch", "f=%llu srt=0x%llx%s",
		                    static_cast<unsigned long long>(Common::Telemetry::g_game_frames.load()),
		                    static_cast<unsigned long long>(srt), line.c_str());
	}
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	vk::Buffer     indirect_buffer = nullptr;
	vk::DeviceSize indirect_offset = 0;
	if (emit.indirect_args != 0) {
		EXIT_IF(mesh_active || emit.indirect_count == 0);
		const uint64_t span = static_cast<uint64_t>(emit.indirect_count - 1u) * emit.indirect_stride +
		                      (draw.IsIndexed() ? 5u : 4u) * sizeof(uint32_t);
		// Compute writes reach indirect reads through the barrier that follows every dispatch.
		auto [args_buffer, args_offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(emit.indirect_args, span, false);
		indirect_buffer = args_buffer->Handle();
		indirect_offset = args_offset;
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	const uint32_t vs_scaled_images = mesh_emulated ? 0u : ScaledImageMask(bindings.vertex[0]);
	uint32_t       ps_scaled_images = state.ps_active ? ScaledImageMask(*bindings.pixel) : 0u;
	{
		auto&      cache        = m_context.GetTextureCache();
		const auto target_scale = [&](ImageId id) {
			const auto* image = id ? cache.m_slot_images.try_get(id) : nullptr;
			return image != nullptr && image->IsScaled();
		};
		bool scaled_target = target_scale(state.depth_info.image_id);
		for (uint32_t i = 0; i < state.color_count; i++) {
			scaled_target |= target_scale(state.color_info[i].image_id);
		}
		if (scaled_target) {
			ps_scaled_images |= ShaderRecompiler::IR::ScaledRenderTargetBit;
		}
	}
	// A draw may be skipped while its pipeline compiles in the background, unless skipping it
	// would lose something for good: shader buffer writes, or a small target (LUTs and similar
	// are often rendered once).
	bool allow_async = !mesh_emulated && !mesh_active;
	for (const auto& stage: vertex_stages) {
		allow_async &= !HasShaderBufferWrites(stage.stage);
	}
	if (state.ps_active) {
		allow_async &= !HasShaderBufferWrites(state.ps_input_info.stage);
	}
	for (uint32_t i = 0; i < state.color_count && allow_async; i++) {
		const auto& extent = state.color_info[i].desc.info.extent;
		allow_async &= static_cast<uint64_t>(extent.width) * extent.height > 512ull * 512ull;
	}
	if (state.color_count == 0 && state.depth_info.image_id) {
		const auto& extent = state.depth_info.desc.info.extent;
		allow_async &= static_cast<uint64_t>(extent.width) * extent.height > 512ull * 512ull;
	}
	auto* pipeline_ptr = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs, vs_scaled_images, ps_scaled_images, allow_async);
	if (pipeline_ptr == nullptr) {
		Common::DebugFlags::FrameTrace("    -> skipped: pipeline compiling in the background");
		return;
	}
	auto& pipeline = *pipeline_ptr;
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);
	if (Common::DebugFlags::FrameTracing()) {
		const auto& shaders = buffer.GetShaders();
		const auto* rt0     = state.color_count != 0 ? &state.color_info[0] : nullptr;
		Common::DebugFlags::FrameTrace(
		    "draw %s n=%u inst=%u vs=0x%llx ps=0x%llx targets=%u rt0=0x%llx %ux%u fmt=%u tile=0x%x",
		    draw.Name(), draw.index_count, draw.instance_count,
		    static_cast<unsigned long long>(shaders.GetVs().gs_regs.data_addr),
		    static_cast<unsigned long long>(shaders.GetPs().ps_regs.data_addr), state.color_count,
		    static_cast<unsigned long long>(rt0 ? rt0->desc.info.data.address : 0),
		    rt0 ? rt0->desc.info.extent.width : 0, rt0 ? rt0->desc.info.extent.height : 0,
		    rt0 ? static_cast<uint32_t>(rt0->desc.info.guest_format) : 0,
		    rt0 ? static_cast<uint32_t>(rt0->desc.info.tile_mode) : 0);
		Common::DebugFlags::FrameTrace("    rendering ds_layout=%d has_depth=%d has_stencil=%d feedback=%u",
		    static_cast<int>(rendering.depth_stencil_attachment.image_layout),
		    rendering.depth_stencil_attachment.has_depth ? 1 : 0,
		    rendering.depth_stencil_attachment.has_stencil ? 1 : 0,
		    static_cast<uint32_t>(feedback_aspects));
		Common::DebugFlags::FrameTrace("    psctl kill=%d zexport=%d early_z=%d",
		    state.ps_input_info.ps_pixel_kill_enable ? 1 : 0,
		    state.ps_input_info.ps_depth_export_enable ? 1 : 0, state.ps_input_info.ps_early_z ? 1 : 0);
		for (int r = 0; r < state.vertex_info[0].resources_num; r++) {
			const auto& res = state.vertex_info[0].resources[r];
			Common::DebugFlags::FrameTrace("    vtx[%d] addr=0x%llx stride=%u records=%u fmt=%u sel=0x%03x", r,
			                               static_cast<unsigned long long>(res.Base48()), res.Stride(),
			                               res.NumRecords(), static_cast<uint32_t>(res.RawFormat()),
			                               res.DstSelXYZW());
		}
		const auto& dc = buffer.GetRegisters().GetDepthControl();
		const auto& di = state.depth_info;
		Common::DebugFlags::FrameTrace(
		    "    depth img=%d id=%u vkfmt=%d addr=0x%llx clr=%d/%d %.3f test=%d write=%d op=%d | guest z=%d zw=%d zfunc=%u st=%d sfunc=%u | vk st=%d fail=%d pass=%d zfail=%d cmp=%d mask=0x%x wmask=0x%x ref=0x%x",
		    di.image_id ? 1 : 0, static_cast<uint32_t>(di.image_id.index),
		    static_cast<int>(di.desc.info.pixel_format), static_cast<unsigned long long>(di.desc.info.data.address),
		    di.depth_clear_enable ? 1 : 0, di.depth_load_clear_enable ? 1 : 0,
		    static_cast<double>(di.depth_clear_value), di.depth_test_enable ? 1 : 0, di.depth_write_enable ? 1 : 0,
		    static_cast<int>(di.depth_compare_op), dc.z_enable ? 1 : 0, dc.z_write_enable ? 1 : 0,
		    dc.zfunc, dc.stencil_enable ? 1 : 0, dc.stencilfunc, di.stencil_test_enable ? 1 : 0,
		    static_cast<int>(di.stencil_front.failOp), static_cast<int>(di.stencil_front.passOp),
		    static_cast<int>(di.stencil_front.depthFailOp), static_cast<int>(di.stencil_front.compareOp),
		    di.stencil_front.compareMask, di.stencil_front.writeMask, di.stencil_front.reference);
	}

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	uint32_t   mesh_index_count = 0;
	Buffer*    mesh_output      = nullptr;
	uint64_t   mesh_index_offset = 0;
	if (mesh_emulated) {
		// Geometry shader without VK_EXT_mesh_shader: the mesh program runs as a compute shader
		// that writes every workgroup's vertex records and triangles, drawn below as a list.
		const auto& mesh          = state.vertex_info[0].mesh;
		const auto  groups        = static_cast<uint64_t>(mesh_groups) * draw.instance_count;
		const auto  record_bytes  = uint64_t {ShaderRecompiler::IR::MeshRecordDwords(
		                                state.vertex_info[0].stage.program->info.outputs)} * 4u;
		const auto  index_bytes   = groups * mesh.max_primitives * 3u * sizeof(uint32_t);
		mesh_index_offset         = (groups * mesh.max_vertices * record_bytes + 255u) & ~uint64_t {255u};
		EXIT_IF(mesh_index_offset > UINT32_MAX || groups * mesh.max_primitives * 3u > UINT32_MAX);
		mesh_output               = &MeshOutputBuffer(mesh_index_offset + index_bytes);
		mesh_index_count          = static_cast<uint32_t>(groups * mesh.max_primitives * 3u);
		auto& compute_pipeline    = m_context.GetPipelineCache().GetMeshComputePipeline(
		    state.vertex_info[0], state.programs.vertex[0], ScaledImageMask(bindings.vertex[0]));
		m_context.GetCommandScheduler().EndRendering();
		vk::MemoryBarrier full {};
		full.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		full.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		// Earlier draws may still read the previous contents of the output buffer.
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                          vk::PipelineStageFlagBits::eAllCommands, {}, 1, &full, 0, nullptr,
		                          0, nullptr);
		// Triangles a workgroup does not emit stay (0, 0, 0): degenerate, never rasterized.
		vk_buffer.fillBuffer(mesh_output->Handle(), mesh_index_offset, index_bytes, 0);
		vk::MemoryBarrier filled {};
		filled.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		filled.dstAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                          vk::PipelineStageFlagBits::eComputeShader, {}, 1, &filled, 0,
		                          nullptr, 0, nullptr);
		PreparedBindings* mesh_stage = &bindings.vertex[0];
		CommitBindings(buffer, vk::PipelineBindPoint::eCompute, compute_pipeline,
		               std::span {&mesh_stage, 1u});
		const auto records = mesh_output->BufferDeviceAddress();
		const uint32_t draw_data[] {
		    draw.index_count,
		    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u), mesh_groups,
		    static_cast<uint32_t>(mesh_index_offset), static_cast<uint32_t>(records),
		    static_cast<uint32_t>(records >> 32u)};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(compute_pipeline.pipeline_layout, vk::ShaderStageFlagBits::eCompute,
		                        0, sizeof(draw_data), draw_data);
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, compute_pipeline.pipeline);
		vk_buffer.dispatch(mesh_groups, draw.instance_count, 1);
		vk::MemoryBarrier written {};
		written.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		written.dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead | vk::AccessFlagBits::eIndexRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eVertexInput, {}, 1, &written, 0,
		                          nullptr, 0, nullptr);
		const vk::DeviceSize zero          = 0;
		const vk::Buffer     output_handle = mesh_output->Handle();
		vk_buffer.bindVertexBuffers(0, 1, &output_handle, &zero);
		vk_buffer.bindIndexBuffer(mesh_output->Handle(), mesh_index_offset, vk::IndexType::eUint32);
		// The graphics pipeline has only the pixel shader's bindings.
		CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages.subspan(1));
	} else {
		CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	}
	if (mesh_active && !mesh_emulated) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u), 0, 0, 0, 0};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eMeshEXT |
		                            vk::ShaderStageFlagBits::eFragment,
		                        0, sizeof(draw_data), draw_data);
	} else if (!mesh_active) {
		CommitIndexBuffer(vk_buffer, index_binding);
	}
	if (const auto planes = vertex_stages.back().user_clip_planes; planes != 0) {
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		                        ShaderRecompiler::IR::PushData::UserClipPlaneDword * sizeof(uint32_t),
		                        std::bit_width(planes) * 4u * sizeof(uint32_t),
		                        buffer.GetRegisters().GetUserClipPlanes().data());
	}

	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	// Bisection aid: `setprop debug.capy.dyn_invalidate 1` re-records all dynamic state per draw.
	static Common::DebugFlags::CachedValue dyn_invalidate("debug.capy.dyn_invalidate", 0);
	if (dyn_invalidate.Get() != 0) {
		dynamic_state.Invalidate();
	}
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), state.depth_info,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);

	LogDrawPhase(draw.Name(), "BeginRendering");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	// `debug.capy.force_split 1`: end the render pass before every draw (tests whether resuming
	// a pass is harmful on the host driver). 2: only draws whose pixel shader hashes to
	// `split_rem` modulo `split_mod` (bisects which draw needs it); 3: only pixel shader `split_ps`; 4: only draws
	// whose first color target is at guest address `split_ps`.
	static Common::DebugFlags::CachedValue force_split("debug.capy.force_split", 0);
	static Common::DebugFlags::CachedValue split_mod("debug.capy.split_mod", 2);
	static Common::DebugFlags::CachedValue split_rem("debug.capy.split_rem", 0);
	static Common::DebugFlags::CachedValue split_ps("debug.capy.split_ps", 0);
	const auto split_mode = force_split.Get();
	const uint64_t split_addr = state.ps_active ? buffer.GetShaders().GetPs().ps_regs.data_addr : 0;
	const uint64_t split_hash = (split_addr >> 8) * 0x9E3779B97F4A7C15ull >> 32;
	if (split_mode == 1 ||
	    (split_mode == 2 && split_mod.Get() > 0 &&
	     static_cast<int64_t>(split_hash % static_cast<uint64_t>(split_mod.Get())) == split_rem.Get()) ||
	    (split_mode == 3 && split_addr == static_cast<uint64_t>(split_ps.Get())) ||
	    (split_mode == 4 && state.color_count > 0 &&
	     state.color_info[0].desc.info.data.address == static_cast<uint64_t>(split_ps.Get()))) {
		m_context.GetCommandScheduler().EndRendering();
	}
	// 6: within the run of consecutive draws to target `split_ps`, split every `split_mod` draws;
	// 7: split only before draw `split_rem` of that run (0-based).
	static uint64_t run_target = 0;
	static int64_t  run_index  = 0;
	const uint64_t  rt0_addr   = state.color_count > 0 ? state.color_info[0].desc.info.data.address : 0;
	// `debug.capy.run_log 1`: log the draw and index count of every run of draws to `split_ps`
	// (tag CapyPS5-Run), to spot frames where the game submits fewer draws.
	static Common::DebugFlags::CachedValue run_log("debug.capy.run_log", 0);
	static uint64_t run_indices = 0;
	if (rt0_addr != run_target) {
#if defined(__ANDROID__)
		if (run_log.Get() != 0 && run_target == static_cast<uint64_t>(split_ps.Get())) {
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Run", "frame %llu draws %lld indices %llu",
			                    static_cast<unsigned long long>(Common::Telemetry::g_game_frames.load()),
			                    static_cast<long long>(run_index + 1),
			                    static_cast<unsigned long long>(run_indices));
		}
#endif
		run_target  = rt0_addr;
		run_index   = 0;
		run_indices = 0;
	} else {
		run_index++;
	}
	run_indices += draw.index_count;
	if (rt0_addr == static_cast<uint64_t>(split_ps.Get()) &&
	    ((split_mode == 6 && split_mod.Get() > 0 && run_index % split_mod.Get() == 0) ||
	     (split_mode == 7 && run_index == split_rem.Get()))) {
		m_context.GetCommandScheduler().EndRendering();
	}
	// `debug.capy.force_split 5`: end the pass when the depth compare direction (less vs greater)
	// flips inside it (Adreno LRZ investigation).
	static int pass_depth_direction = 0;
	if (!m_context.GetCommandScheduler().Current().IsRendering()) {
		pass_depth_direction = 0;
	}
	if (state.depth_info.depth_test_enable) {
		int direction = 0;
		switch (state.depth_info.depth_compare_op) {
			case vk::CompareOp::eLess:
			case vk::CompareOp::eLessOrEqual: direction = 1; break;
			case vk::CompareOp::eGreater:
			case vk::CompareOp::eGreaterOrEqual: direction = 2; break;
			default: break;
		}
		if (direction != 0) {
			if (split_mode == 5 && pass_depth_direction != 0 && pass_depth_direction != direction) {
				m_context.GetCommandScheduler().EndRendering();
			}
			pass_depth_direction = direction;
		}
	}
	if (GpuProfiler::Enabled() && !m_context.GetCommandScheduler().Current().IsRendering()) {
		GpuProfiler::SetPassLabel(rt0_addr, rendering.width, rendering.height, split_addr);
	}
	// Without the global barriers of partial flushes (`debug.capy.event_barriers 0`), a draw that
	// writes memory waits for earlier accesses to it, as dispatches do. Only outside a pass: an
	// open pass is not split for it.
	static Common::DebugFlags::CachedValue event_barriers("debug.capy.event_barriers", 1);
	if (event_barriers.Get() == 0 && !m_context.GetCommandScheduler().Current().IsRendering()) {
		vk::PipelineStageFlags write_stages = {};
		for (const auto& stage: vertex_stages) {
			if (HasShaderBufferWrites(stage.stage)) {
				write_stages |= ShaderPipelineStages(ProgramShaderStage(*stage.stage.program));
			}
		}
		if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
			write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
		}
		if (write_stages) {
			ShaderWriteHazardBarrier(vk_buffer, write_stages);
		}
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	GpuProfiler::NoteDraw();
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	// Bisection aid: `setprop debug.capy.clear_depth_ps 0x<ps>` clears the depth target to
	// `clear_depth_milli`/1000 right after each depth-writing ALWAYS draw of that pixel shader
	// (`clear_depth_pre 1`: right before it instead, to see what the draw itself writes).
	static Common::DebugFlags::CachedValue clear_depth_ps("debug.capy.clear_depth_ps", 0);
	static Common::DebugFlags::CachedValue clear_depth_milli("debug.capy.clear_depth_milli", 0);
	static Common::DebugFlags::CachedValue clear_depth_pre("debug.capy.clear_depth_pre", 0);
	const bool clear_depth_probe =
	    clear_depth_ps.Get() != 0 && rendering.depth_stencil_attachment.has_depth &&
	    state.depth_info.depth_write_enable && state.depth_info.depth_compare_op == vk::CompareOp::eAlways &&
	    static_cast<uint64_t>(clear_depth_ps.Get()) == split_addr;
	const auto probe_clear_depth = [&] {
		vk::ClearAttachment clear {};
		clear.aspectMask                    = vk::ImageAspectFlagBits::eDepth;
		clear.clearValue.depthStencil.depth = static_cast<float>(clear_depth_milli.Get()) / 1000.0f;
		vk::ClearRect rect {};
		rect.rect.extent = vk::Extent2D {rendering.width, rendering.height};
		rect.layerCount  = 1;
		vk_buffer.clearAttachments(1, &clear, 1, &rect);
	};
	if (clear_depth_probe && clear_depth_pre.Get() != 0) {
		probe_clear_depth();
	}
	// Adreno: a pass that loads depth and starts with a depth-writing ALWAYS draw lost that draw's
	// depth writes unless something cleared depth in the pass first (Ratchet & Clank: Size
	// Matters' per-frame depth clear draw kept the previous frame's depth, so the scene failed its
	// GREATER test and showed the fog colour with trails). Clearing one pixel, which the draw
	// then overwrites, is enough. `debug.capy.depth_kick 0` disables it.
	static Common::DebugFlags::CachedValue depth_kick("debug.capy.depth_kick", 1);
	if (depth_kick.Get() != 0 && rendering.depth_stencil_attachment.has_depth &&
	    state.depth_info.depth_write_enable && state.depth_info.depth_compare_op == vk::CompareOp::eAlways) {
		vk::ClearAttachment clear {};
		clear.aspectMask                    = vk::ImageAspectFlagBits::eDepth;
		clear.clearValue.depthStencil.depth = 0.0f;
		vk::ClearRect rect {};
		rect.rect.extent = vk::Extent2D {1, 1};
		rect.layerCount  = 1;
		vk_buffer.clearAttachments(1, &clear, 1, &rect);
	}
	if (mesh_emulated) {
		vk_buffer.drawIndexed(mesh_index_count, 1, 0, 0, 0);
	} else if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit, indirect_buffer, indirect_offset);
	}
	if (clear_depth_probe && clear_depth_pre.Get() == 0) {
		probe_clear_depth();
	}

	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(ProgramShaderStage(*stage.stage.program));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	// Draws whose shaders write memory need a barrier before later work reads it. Ending the pass
	// for it split Stray's 4K G-buffer pass (7 targets) at every draw; dependent draws inside a
	// pass are ordered by the game's own wait events, so the barrier could wait for the pass end.
	// Deferring it blindly (1) made Stray's layers flicker: some draws read what earlier draws of
	// the pass wrote. 2 (default) defers it and records the written ranges, so a later draw of the
	// pass that reads one ends the pass first. 0 ends the pass after every such draw.
	static Common::DebugFlags::CachedValue defer_write_barrier("debug.capy.defer_write_barrier", 2);
	if (shader_write_stages) {
		if (defer_write_barrier.Get() != 0 && m_context.GetCommandScheduler().Current().IsRendering()) {
			m_context.GetCommandScheduler().Current().DeferGlobalBarrier();
			if (defer_write_barrier.Get() == 2) {
				for (const auto* stage: stages) {
					const auto& program = *stage->runtime->program;
					for (const auto& resource: program.info.buffers) {
						if (resource.written && resource.descriptor_index < stage->buffer_sources.size()) {
							const auto& source = stage->buffer_sources[resource.descriptor_index];
							if (source.size != 0) {
								m_pass_writes.emplace_back(source.address, source.size);
							}
						}
					}
				}
			}
		} else {
			m_context.GetCommandScheduler().EndRendering();
			ShaderWriteBarrier(vk_buffer, shader_write_stages);
		}
	}
	// `setprop debug.capy.dump_after N`: during a frame trace, write the color targets back to
	// guest memory right after the Nth draw (1-based, as numbered by the trace) of the frame.
	if (Common::DebugFlags::FrameTracing()) {
		static Common::DebugFlags::CachedValue dump_after("debug.capy.dump_after", 0);
		const auto index = Common::DebugFlags::g_trace_draws.fetch_add(1) + 1;
		Common::DebugFlags::FrameTrace("  draw #%u", index);
		if (dump_after.Get() != 0 && static_cast<int64_t>(index) == dump_after.Get()) {
			m_context.GetCommandScheduler().EndRendering();
			for (uint32_t i = 0; i < state.color_count; i++) {
				const bool ok = m_context.GetTextureCache().DebugDownloadImage(state.color_info[i].image_id);
				Common::DebugFlags::FrameTrace("  dump target slot=%u addr=0x%llx -> %d",
				    state.color_info[i].target_slot,
				    static_cast<unsigned long long>(state.color_info[i].desc.info.data.address), ok ? 1 : 0);
			}
			if (state.depth_info.image_id) {
				const bool ok = m_context.GetTextureCache().DebugDownloadImage(state.depth_info.image_id);
				Common::DebugFlags::FrameTrace("  dump depth-after -> %d", ok ? 1 : 0);
			}
		}
#if defined(__ANDROID__)
		// `debug.capy.dump_files N`: save the first color target and the depth target of traced
		// draws [dump_from, dump_from + N) linearly to <files>/watch/f<draw>.img.
		static Common::DebugFlags::CachedValue dump_from("debug.capy.dump_from", 1);
		static Common::DebugFlags::CachedValue dump_files("debug.capy.dump_files", 0);
		if (dump_files.Get() > 0 && static_cast<int64_t>(index) >= dump_from.Get() &&
		    static_cast<int64_t>(index) < dump_from.Get() + dump_files.Get() && state.color_count > 0) {
			const auto dir = AndroidGetFilesDir() + "/watch";
			Common::File::CreateDirectories(dir);
			char name[32];
			std::snprintf(name, sizeof(name), "/f%04u.img", index);
			m_context.GetTextureCache().DebugSaveImageLinear(state.color_info[0].image_id, dir + name);
			if (state.depth_info.image_id) {
				std::snprintf(name, sizeof(name), "/f%04u_d.img", index);
				m_context.GetTextureCache().DebugSaveImageLinear(state.depth_info.image_id, dir + name);
			}
		}
#endif
	}
#if defined(__ANDROID__)
	// `debug.capy.watch_dump N` with watch_ps: save the first color target of the next N draws of
	// that pixel shader linearly to <files>/watch/<game frame>_<n>.img (no frame trace needed).
	static Common::DebugFlags::CachedValue watch_ps_dump("debug.capy.watch_ps", 0);
	static Common::DebugFlags::CachedValue watch_dump("debug.capy.watch_dump", 0);
	static int64_t watch_dump_left = 0;
	static int64_t watch_dump_seen = 0;
	if (watch_dump.Get() != watch_dump_seen) {
		watch_dump_seen = watch_dump.Get();
		watch_dump_left = watch_dump_seen;
	}
	// watch_ps 1 instead dumps every `watch_step`-th draw of the run of draws to target `split_ps`
	// and also saves its depth target (<name>_d.img).
	static Common::DebugFlags::CachedValue watch_step("debug.capy.watch_step", 1);
	const bool watch_run = watch_ps_dump.Get() == 1 && rt0_addr == static_cast<uint64_t>(split_ps.Get()) &&
	                       watch_step.Get() > 0 && run_index % watch_step.Get() == 0;
	if (watch_dump_left > 0 && state.ps_active && state.color_count > 0 &&
	    (watch_run ||
	     static_cast<uint64_t>(watch_ps_dump.Get()) == buffer.GetShaders().GetPs().ps_regs.data_addr)) {
		const auto dir = AndroidGetFilesDir() + "/watch";
		Common::File::CreateDirectories(dir);
		char name[64];
		std::snprintf(name, sizeof(name), "/%06llu_%03lld.img",
		              static_cast<unsigned long long>(Common::Telemetry::g_game_frames.load()),
		              static_cast<long long>(watch_run ? run_index : watch_dump_seen - watch_dump_left));
		m_context.GetTextureCache().DebugSaveImageLinear(state.color_info[0].image_id, dir + name);
		if (watch_run && state.depth_info.image_id) {
			std::snprintf(name, sizeof(name), "/%06llu_%03lld_d.img",
			              static_cast<unsigned long long>(Common::Telemetry::g_game_frames.load()),
			              static_cast<long long>(run_index));
			m_context.GetTextureCache().DebugSaveImageLinear(state.depth_info.image_id, dir + name);
		}
		watch_dump_left--;
	}
#endif
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

uint32_t RenderExecutor::ScaledImageMask(const PreparedBindings& bindings) {
	auto&    cache = m_context.GetTextureCache();
	uint32_t mask  = 0;
	for (uint32_t i = 0; i < bindings.images.size() && i < ShaderRecompiler::IR::ScaledImageResources; i++) {
		const auto* image = cache.m_slot_images.try_get(bindings.images[i].image_id);
		if (image != nullptr && image->IsScaled()) {
			mask |= 1u << i;
		}
	}
	return mask;
}

Buffer& RenderExecutor::MeshOutputBuffer(uint64_t size) {
	if (m_mesh_output == nullptr || m_mesh_output->Size() < size) {
		if (m_mesh_output != nullptr) {
			m_retired_mesh_outputs.push_back(std::move(m_mesh_output));
		}
		const auto capacity = std::max<uint64_t>(std::bit_ceil(size), 4ull << 20u);
		m_mesh_output = std::make_unique<Buffer>(
		    m_context.GetGraphics(), m_context.GetCommandScheduler(), MemoryUsage::DeviceLocal, 0,
		    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, capacity);
	}
	return *m_mesh_output;
}

// `setprop debug.capy.trace_ps <ps address>`: frame-trace only the next 8 draws with that pixel
// shader (tag CapyPS5-Frame; with dump_ps set to the same address it also disassembles it).
class TracePsScope {
public:
	explicit TracePsScope(uint64_t ps_addr) {
		static Common::DebugFlags::CachedValue trace_ps("debug.capy.trace_ps", 0);
		static int64_t                         armed = 0;
		static uint32_t                        left  = 0;
		if (trace_ps.Get() != armed) {
			armed = trace_ps.Get();
			left  = 8;
		}
		if (armed != 0 && left != 0 && !Common::DebugFlags::FrameTracing() &&
		    ps_addr == static_cast<uint64_t>(armed)) {
			left--;
			m_active = true;
			Common::DebugFlags::g_trace_lines.store(0, std::memory_order_relaxed);
			Common::DebugFlags::g_trace_state.store(1, std::memory_order_relaxed);
			Common::DebugFlags::FrameTrace("---- trace_ps draw ----");
		}
	}
	~TracePsScope() {
		if (m_active) {
			Common::DebugFlags::g_trace_state.store(0, std::memory_order_relaxed);
		}
	}
	KYTY_CLASS_NO_COPY(TracePsScope);

private:
	bool m_active = false;
};

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	TracePsScope trace_ps_scope(sh_ctx.GetPs().ps_regs.data_addr);

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	Common::DebugFlags::FrameTrace("submit DrawIndex n=%u inst=%u ps=0x%llx indirect=%d", args.index_count,
	                               args.instance_count,
	                               static_cast<unsigned long long>(sh_ctx.GetPs().ps_regs.data_addr),
	                               args.offset_source == DrawOffsetSource::IndirectArgs ? 1 : 0);
	if (args.index_count == 0 || args.instance_count == 0) {
		Common::DebugFlags::FrameTrace("  skipped: empty");
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		Common::DebugFlags::FrameTrace("  skipped: metadata/copy/resolve op");
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		Common::DebugFlags::FrameTrace("  skipped: no vertex shader");
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		Common::DebugFlags::FrameTrace("  skipped: topology");
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance,
	                        args.indirect_vertex_sgpr, args.indirect_instance_sgpr};
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		Common::DebugFlags::FrameTrace("  skipped: no render state");
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);

	DrawEmitInfo emit {};
	// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.vertex_offset  = native_indirect ? 0 : vertex_offset + args.base_vertex;
	emit.first_instance = native_indirect ? 0 : instance_offset;
	emit.indirect_args   = args.indirect_args;
	emit.indirect_count  = args.indirect_count;
	emit.indirect_stride = args.indirect_stride;

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart);
	ResetBindings();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	TracePsScope trace_ps_scope(buffer.GetShaders().GetPs().ps_regs.data_addr);
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance,
	                         args.indirect_vertex_sgpr, args.indirect_instance_sgpr};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		ResetBindings();
		return;
	}
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		ResetBindings();
		return;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);
	DrawEmitInfo emit {};
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.first_vertex = native_indirect ? 0 :
	    static_cast<uint32_t>(vertex_offset + static_cast<int32_t>(args.first_vertex));
	emit.first_instance = native_indirect ? 0 : instance_offset;
	emit.indirect_args   = args.indirect_args;
	emit.indirect_count  = args.indirect_count;
	emit.indirect_stride = args.indirect_stride;

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false);
	ResetBindings();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
