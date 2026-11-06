// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/image/bcDecoder.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugFlags.h"
#include "gpu_tiler_shaders/bcn_decode_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

namespace {

struct DecodePush {
	uint32_t blocks_x;
	uint32_t blocks_y;
	uint32_t slices;
	uint32_t format;
	uint32_t is_signed;
	uint32_t input_offset;  // in words
	uint32_t output_offset; // in words
};

} // namespace

std::optional<BcDecodedFormat> BcDecodedFormatOf(vk::Format format) noexcept {
	using F = vk::Format;
	switch (format) {
		case F::eBc1RgbUnormBlock:
		case F::eBc1RgbaUnormBlock: return BcDecodedFormat {1, false, F::eR8G8B8A8Unorm, 4};
		case F::eBc1RgbSrgbBlock:
		case F::eBc1RgbaSrgbBlock: return BcDecodedFormat {1, false, F::eR8G8B8A8Srgb, 4};
		case F::eBc2UnormBlock: return BcDecodedFormat {2, false, F::eR8G8B8A8Unorm, 4};
		case F::eBc2SrgbBlock: return BcDecodedFormat {2, false, F::eR8G8B8A8Srgb, 4};
		case F::eBc3UnormBlock: return BcDecodedFormat {3, false, F::eR8G8B8A8Unorm, 4};
		case F::eBc3SrgbBlock: return BcDecodedFormat {3, false, F::eR8G8B8A8Srgb, 4};
		case F::eBc4UnormBlock: return BcDecodedFormat {4, false, F::eR8Unorm, 1};
		case F::eBc4SnormBlock: return BcDecodedFormat {4, true, F::eR8Snorm, 1};
		case F::eBc5UnormBlock: return BcDecodedFormat {5, false, F::eR8G8Unorm, 2};
		case F::eBc5SnormBlock: return BcDecodedFormat {5, true, F::eR8G8Snorm, 2};
		case F::eBc6HUfloatBlock: return BcDecodedFormat {6, false, F::eR16G16B16A16Sfloat, 8};
		case F::eBc6HSfloatBlock: return BcDecodedFormat {6, true, F::eR16G16B16A16Sfloat, 8};
		case F::eBc7UnormBlock: return BcDecodedFormat {7, false, F::eR8G8B8A8Unorm, 4};
		case F::eBc7SrgbBlock: return BcDecodedFormat {7, false, F::eR8G8B8A8Srgb, 4};
		default: return std::nullopt;
	}
}

BcDecoder::BcDecoder(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
	for (uint32_t index = 0; index < bindings.size(); index++) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1,
		                   vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr, &m_descriptor_layout),
	    "create BcDecoder descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(DecodePush)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &m_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create BcDecoder pipeline layout");

	const auto module = CompileSPV({BCN_DECODE_SPV, std::size(BCN_DECODE_SPV)}, m_graphics.device);
	vk::ComputePipelineCreateInfo create {};
	create.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	create.stage.module = module;
	create.stage.pName  = "main";
	create.layout       = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create BcDecoder pipeline");
}

BcDecoder::~BcDecoder() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

std::optional<vk::BufferImageCopy>
BcDecoder::Decode(vk::CommandBuffer command, vk::Buffer source, uint64_t source_offset,
                  uint64_t source_size, const BcDecodedFormat& format,
                  const vk::BufferImageCopy& copy, vk::Buffer& decoded) {
	if (format.shader_format == 6) {
		Common::DebugFlags::NoteOnce("CapyPS5-Bcn", 0x424336ull,
		                             "BC6H textures cannot be decoded yet; they stay black");
		return std::nullopt;
	}
	const auto& limits      = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  width       = copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
	const auto  height      = copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
	const auto  blocks_x    = (width + 3) / 4;
	const auto  blocks_y    = (height + 3) / 4;
	const auto  slices      = copy.imageSubresource.layerCount * copy.imageExtent.depth;
	const auto  block_bytes = format.shader_format == 1 || format.shader_format == 4 ? 8u : 16u;
	const uint64_t slice_blocks = uint64_t {blocks_x} * blocks_y;
	const uint64_t slice_in     = slice_blocks * block_bytes;
	const uint64_t slice_out    = slice_blocks * 16u * format.texel_bytes;
	if (slice_blocks == 0 || slices == 0 || copy.bufferOffset < source_offset ||
	    copy.bufferOffset + slice_in * slices > source_offset + source_size) {
		Common::DebugFlags::NoteOnce("CapyPS5-Bcn", 0x424372616e6765ull,
		                             "BCn upload outside its staging range skipped");
		return std::nullopt;
	}

	// One dispatch covers as many whole slices as the group-count and buffer-range limits allow.
	const uint64_t alignment   = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t max_range   = limits.maxStorageBufferRange - alignment;
	const uint64_t max_blocks  = uint64_t {limits.maxComputeWorkGroupCount[0]} * 64u;
	const uint64_t chunk_limit = std::min({max_blocks / slice_blocks, max_range / slice_out,
	                                       max_range / slice_in, uint64_t {slices}});
	if (chunk_limit == 0) {
		Common::DebugFlags::NoteOnce("CapyPS5-Bcn", 0x424373697a65ull,
		                             "BCn slice of %ux%u blocks is too large to decode",
		                             blocks_x, blocks_y);
		return std::nullopt;
	}

	const uint64_t output_size = Common::AlignUp(slice_out * slices, 4);
	if (!m_output || m_output->Size() < output_size) {
		if (m_output) {
			m_scheduler.DeferOperation([old = std::move(m_output)]() mutable { old.reset(); });
		}
		m_output = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
		    output_size);
	}

	// The blocks were written by a transfer or the host; the output may still be read by the
	// previous decode's copy.
	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);

	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	for (uint64_t first = 0; first < slices; first += chunk_limit) {
		const auto     count      = static_cast<uint32_t>(std::min<uint64_t>(chunk_limit, slices - first));
		const uint64_t in_start   = copy.bufferOffset + first * slice_in;
		const uint64_t out_start  = first * slice_out;
		const uint64_t in_base    = Common::AlignDown(in_start, alignment);
		const uint64_t out_base   = Common::AlignDown(out_start, alignment);
		const vk::DescriptorBufferInfo input {
		    source, in_base, Common::AlignUp(in_start - in_base + slice_in * count, 4)};
		const vk::DescriptorBufferInfo output {
		    m_output->Handle(), out_base, Common::AlignUp(out_start - out_base + slice_out * count, 4)};
		std::array<vk::WriteDescriptorSet, 2> writes {};
		for (uint32_t index = 0; index < writes.size(); index++) {
			writes[index].dstBinding      = index;
			writes[index].descriptorCount = 1;
			writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
			writes[index].pBufferInfo     = index == 0 ? &input : &output;
		}
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
		                             static_cast<uint32_t>(writes.size()), writes.data());
		const DecodePush push {blocks_x,
		                       blocks_y,
		                       count,
		                       format.shader_format,
		                       format.is_signed ? 1u : 0u,
		                       static_cast<uint32_t>((in_start - in_base) / 4),
		                       static_cast<uint32_t>((out_start - out_base) / 4)};
		command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
		                      &push);
		const uint64_t blocks = slice_blocks * count;
		command.dispatch(static_cast<uint32_t>((blocks + 63) / 64), 1, 1);
	}

	vk::MemoryBarrier2 after {};
	after.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask  = vk::PipelineStageFlagBits2::eCopy;
	after.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	dependency.pMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);

	decoded                  = m_output->Handle();
	auto texels              = copy;
	texels.bufferOffset      = 0;
	texels.bufferRowLength   = blocks_x * 4;
	texels.bufferImageHeight = blocks_y * 4;
	return texels;
}

} // namespace Libs::Graphics
