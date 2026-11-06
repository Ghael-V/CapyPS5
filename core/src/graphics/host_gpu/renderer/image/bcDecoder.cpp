// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/image/bcDecoder.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugFlags.h"
#include "gpu_tiler_shaders/bcn_decode_spv.h"
#include "gpu_tiler_shaders/bcn_verify_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"

#include <algorithm>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <array>
#include <atomic>
#include <cstring>

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
	uint32_t debug_mode;
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
	for (uint32_t format = 1; format < m_pipelines.size(); format++) {
		const vk::SpecializationMapEntry entry {0, 0, sizeof(uint32_t)};
		const vk::SpecializationInfo     specialization {1, &entry, sizeof(format), &format};
		vk::ComputePipelineCreateInfo    create {};
		create.stage.stage               = vk::ShaderStageFlagBits::eCompute;
		create.stage.module              = module;
		create.stage.pName               = "main";
		create.stage.pSpecializationInfo = &specialization;
		create.layout                    = m_pipeline_layout;
		const auto result = m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr,
		                                                             &m_pipelines[format]);
		if (result != vk::Result::eSuccess) {
			m_graphics.device.destroyShaderModule(module, nullptr);
			RequireVulkanSuccess(result, "create BcDecoder pipeline");
		}
	}
	m_graphics.device.destroyShaderModule(module, nullptr);
}

BcDecoder::~BcDecoder() {
	for (const auto pipeline: m_pipelines) {
		if (pipeline) {
			m_graphics.device.destroyPipeline(pipeline, nullptr);
		}
	}
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	if (m_verify_pipeline) {
		m_graphics.device.destroySampler(m_verify_sampler, nullptr);
		m_graphics.device.destroyPipeline(m_verify_pipeline, nullptr);
		m_graphics.device.destroyPipelineLayout(m_verify_pipeline_layout, nullptr);
		m_graphics.device.destroyDescriptorSetLayout(m_verify_descriptor_layout, nullptr);
	}
}

std::optional<vk::BufferImageCopy>
BcDecoder::Decode(vk::CommandBuffer command, vk::Buffer source, uint64_t source_offset,
                  uint64_t source_size, const BcDecodedFormat& format,
                  const vk::BufferImageCopy& copy, vk::Buffer& decoded) {
	Common::DebugFlags::NoteOnce("CapyPS5-Bcn", 0x42430000ull | format.shader_format,
	                             "decoding BC%u textures", format.shader_format);
	// Bisecting aids, bit n = BCn: `setprop debug.capy.bc_decode_skip` leaves those images black
	// without decoding; `debug.capy.bc_decode_zero` runs the decoder but writes zeros.
	static Common::DebugFlags::CachedValue skip("debug.capy.bc_decode_skip", 0);
	static Common::DebugFlags::CachedValue zero("debug.capy.bc_decode_zero", 0);
	if (((skip.Get() >> format.shader_format) & 1) != 0) {
		return std::nullopt;
	}
	const auto debug_mode = ((zero.Get() >> format.shader_format) & 1) != 0 ? 1u : 0u;
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

#if defined(__ANDROID__)
	// `setprop debug.capy.bc_decode_log 1`: log every decode (CapyPS5-BcnLog).
	static Common::DebugFlags::CachedValue log_decodes("debug.capy.bc_decode_log", 0);
	if (log_decodes.Get() != 0) {
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-BcnLog",
		                    "BC%u src=%p off=0x%llx size=0x%llx copy_off=0x%llx row=%u h=%u ext=%ux%ux%u "
		                    "mip=%u layers=%u+%u blocks=%ux%u slices=%u chunk=%llu",
		                    format.shader_format, static_cast<void*>(static_cast<VkBuffer>(source)),
		                    static_cast<unsigned long long>(source_offset),
		                    static_cast<unsigned long long>(source_size),
		                    static_cast<unsigned long long>(copy.bufferOffset), copy.bufferRowLength,
		                    copy.bufferImageHeight, copy.imageExtent.width, copy.imageExtent.height,
		                    copy.imageExtent.depth, copy.imageSubresource.mipLevel,
		                    copy.imageSubresource.baseArrayLayer, copy.imageSubresource.layerCount,
		                    blocks_x, blocks_y, slices, static_cast<unsigned long long>(chunk_limit));
	}
#endif
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

	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipelines[format.shader_format]);
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
		                       static_cast<uint32_t>((out_start - out_base) / 4),
		                       debug_mode};
		command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
		                      &push);
		const uint64_t blocks = slice_blocks * count;
		const auto groups = static_cast<uint32_t>((blocks + 63) / 64);
		// Profiled as compute shader 0xBC0n (`debug.capy.gpu_profile`).
		const bool profile = GpuProfiler::Enabled();
		if (profile) {
			GpuProfiler::BeginDispatch(m_graphics, command, 0xBC00u | format.shader_format, groups);
		}
		command.dispatch(groups, 1, 1);
		if (profile) {
			GpuProfiler::EndScope(command);
		}
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

namespace {

struct VerifyPush {
	uint32_t width;
	uint32_t height;
	uint32_t row_texels;
	uint32_t bytes_per_texel;
	uint32_t blocks_x;
	uint32_t block_words;
	uint32_t input_offset;
	uint32_t tolerance;
};

// The GPU decodes sRGB blocks to linear values; the decoded images hold the raw bytes.
vk::Format UnormBlockFormat(vk::Format format) noexcept {
	using F = vk::Format;
	switch (format) {
		case F::eBc1RgbSrgbBlock: return F::eBc1RgbUnormBlock;
		case F::eBc1RgbaSrgbBlock: return F::eBc1RgbaUnormBlock;
		case F::eBc2SrgbBlock: return F::eBc2UnormBlock;
		case F::eBc3SrgbBlock: return F::eBc3UnormBlock;
		case F::eBc7SrgbBlock: return F::eBc7UnormBlock;
		default: return format;
	}
}

} // namespace

bool BcDecoder::VerifyEnabled() {
	static Common::DebugFlags::CachedValue verify("debug.capy.bc_verify", 0);
	return verify.Get() != 0;
}

void BcDecoder::CreateVerifyPipeline() {
	std::array<vk::DescriptorSetLayoutBinding, 4> bindings {};
	for (uint32_t index = 0; index < bindings.size(); index++) {
		bindings[index] = {index,
		                   index == 0 ? vk::DescriptorType::eCombinedImageSampler
		                              : vk::DescriptorType::eStorageBuffer,
		                   1, vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_verify_descriptor_layout),
	                     "create BcDecoder verify descriptor layout");
	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(VerifyPush)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &m_verify_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_verify_pipeline_layout),
	    "create BcDecoder verify pipeline layout");
	const auto module = CompileSPV({BCN_VERIFY_SPV, std::size(BCN_VERIFY_SPV)}, m_graphics.device);
	vk::ComputePipelineCreateInfo create {};
	create.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	create.stage.module = module;
	create.stage.pName  = "main";
	create.layout       = m_verify_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_verify_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create BcDecoder verify pipeline");
	vk::SamplerCreateInfo sampler_info {};
	sampler_info.magFilter    = vk::Filter::eNearest;
	sampler_info.minFilter    = vk::Filter::eNearest;
	sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	RequireVulkanSuccess(m_graphics.device.createSampler(&sampler_info, nullptr, &m_verify_sampler),
	                     "create BcDecoder verify sampler");
}

void BcDecoder::Verify(vk::CommandBuffer command, vk::Buffer source, vk::Format block_format,
                       const BcDecodedFormat& format, const vk::BufferImageCopy& copy,
                       const vk::BufferImageCopy& texels) {
	// Signed and BC6H texels are not plain bytes; arrays and volumes are left out for brevity.
	if (format.is_signed || format.shader_format == 6 || copy.imageSubresource.layerCount != 1 ||
	    copy.imageExtent.depth != 1) {
		return;
	}
	if (!m_verify_pipeline) {
		CreateVerifyPipeline();
	}
	const auto native_format = UnormBlockFormat(block_format);
	auto       image         = std::make_unique<VulkanImage>();
	vk::ImageCreateInfo image_info {};
	image_info.imageType     = vk::ImageType::e2D;
	image_info.format        = native_format;
	image_info.extent        = vk::Extent3D {copy.imageExtent.width, copy.imageExtent.height, 1};
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.samples       = vk::SampleCountFlagBits::e1;
	image_info.tiling        = vk::ImageTiling::eOptimal;
	image_info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
	image_info.initialLayout = vk::ImageLayout::eUndefined;
	if (!m_graphics.CreateImage(image_info, *image)) {
		return;
	}
	vk::ImageViewCreateInfo view_info {};
	view_info.image            = image->image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = native_format;
	view_info.subresourceRange = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::ImageView view         = nullptr;
	if (m_graphics.device.createImageView(&view_info, nullptr, &view) != vk::Result::eSuccess) {
		m_graphics.DeleteImage(*image);
		return;
	}
	constexpr uint64_t ResultSize = 64;
	auto result = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
	                                       vk::BufferUsageFlagBits::eStorageBuffer, ResultSize);
	std::memset(result->Mapped().data(), 0, ResultSize);
	result->Flush(0, ResultSize);

	vk::MemoryBarrier2 memory {};
	memory.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	memory.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	memory.dstStageMask =
	    vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eComputeShader;
	memory.dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderRead;
	vk::ImageMemoryBarrier2 to_copy {};
	to_copy.srcStageMask        = vk::PipelineStageFlagBits2::eNone;
	to_copy.dstStageMask        = vk::PipelineStageFlagBits2::eCopy;
	to_copy.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	to_copy.oldLayout           = vk::ImageLayout::eUndefined;
	to_copy.newLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_copy.image               = image->image;
	to_copy.subresourceRange    = view_info.subresourceRange;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount      = 1;
	dependency.pMemoryBarriers         = &memory;
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &to_copy;
	command.pipelineBarrier2(dependency);
	auto native_copy             = copy;
	native_copy.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	native_copy.imageOffset      = vk::Offset3D {0, 0, 0};
	command.copyBufferToImage(source, image->image, vk::ImageLayout::eTransferDstOptimal, 1,
	                          &native_copy);
	auto to_read          = to_copy;
	to_read.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	to_read.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	to_read.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
	to_read.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	to_read.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
	dependency.memoryBarrierCount   = 0;
	dependency.pMemoryBarriers      = nullptr;
	dependency.pImageMemoryBarriers = &to_read;
	command.pipelineBarrier2(dependency);

	const auto&    limits    = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto width  = copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
	const auto height = copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
	const auto blocks_x    = (width + 3) / 4;
	const auto blocks_y    = (height + 3) / 4;
	const auto block_bytes = format.shader_format == 1 || format.shader_format == 4 ? 8u : 16u;
	const uint64_t in_base = Common::AlignDown(copy.bufferOffset, alignment);
	const vk::DescriptorImageInfo  image_descriptor {m_verify_sampler, view,
	                                                 vk::ImageLayout::eShaderReadOnlyOptimal};
	const vk::DescriptorBufferInfo texel_descriptor {m_output->Handle(), 0, VK_WHOLE_SIZE};
	const vk::DescriptorBufferInfo block_descriptor {
	    source, in_base,
	    Common::AlignUp(copy.bufferOffset - in_base + uint64_t {blocks_x} * blocks_y * block_bytes,
	                    4)};
	const vk::DescriptorBufferInfo result_descriptor {result->Handle(), 0, ResultSize};
	std::array<vk::WriteDescriptorSet, 4> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = index == 0 ? vk::DescriptorType::eCombinedImageSampler
		                                           : vk::DescriptorType::eStorageBuffer;
	}
	writes[0].pImageInfo  = &image_descriptor;
	writes[1].pBufferInfo = &texel_descriptor;
	writes[2].pBufferInfo = &block_descriptor;
	writes[3].pBufferInfo = &result_descriptor;
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_verify_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_verify_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	const VerifyPush push {copy.imageExtent.width,
	                       copy.imageExtent.height,
	                       texels.bufferRowLength,
	                       format.texel_bytes,
	                       blocks_x,
	                       block_bytes / 4,
	                       static_cast<uint32_t>((copy.bufferOffset - in_base) / 4),
	                       2};
	command.pushConstants(m_verify_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(push), &push);
	command.dispatch((copy.imageExtent.width + 7) / 8, (copy.imageExtent.height + 7) / 8, 1);
	vk::MemoryBarrier2 to_host {};
	to_host.srcStageMask               = vk::PipelineStageFlagBits2::eComputeShader;
	to_host.srcAccessMask              = vk::AccessFlagBits2::eShaderWrite;
	to_host.dstStageMask               = vk::PipelineStageFlagBits2::eHost;
	to_host.dstAccessMask              = vk::AccessFlagBits2::eHostRead;
	dependency.memoryBarrierCount      = 1;
	dependency.pMemoryBarriers         = &to_host;
	dependency.imageMemoryBarrierCount = 0;
	dependency.pImageMemoryBarriers    = nullptr;
	command.pipelineBarrier2(dependency);

	m_scheduler.DeferOperation([graphics = &m_graphics, image = std::move(image), view,
	                            result = std::move(result), bc = format.shader_format, block_format,
	                            mip = copy.imageSubresource.mipLevel,
	                            extent = copy.imageExtent]() mutable {
		result->Invalidate(0, ResultSize);
		std::array<uint32_t, 10> values {};
		std::memcpy(values.data(), result->Mapped().data(), sizeof(values));
		static std::atomic<uint32_t> verified {0};
		static std::atomic<uint32_t> failed {0};
		const auto total = ++verified;
		if (values[0] != 0 && ++failed <= 64) {
#if defined(__ANDROID__)
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-BcnVerify",
			                    "BC%u (vk %d) mip %u %ux%u: %u texels differ; first (%u,%u) "
			                    "ours=%08x native=%08x block=%08x %08x %08x %08x",
			                    bc, static_cast<int>(block_format), mip, extent.width,
			                    extent.height, values[0], values[2], values[3], values[4],
			                    values[5], values[6], values[7], values[8], values[9]);
#endif
		}
		if (total % 200 == 0) {
#if defined(__ANDROID__)
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-BcnVerify",
			                    "%u uploads verified, %u differ", total, failed.load());
#endif
		}
		graphics->device.destroyImageView(view, nullptr);
		graphics->DeleteImage(*image);
	});
}

} // namespace Libs::Graphics
