// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/presentation/postProcess.h"

#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "post_process_shaders/pp_bicubic_spv.h"
#include "post_process_shaders/pp_fsr_easu_fp16_spv.h"
#include "post_process_shaders/pp_fsr_easu_spv.h"
#include "post_process_shaders/pp_fsr_rcas_fp16_spv.h"
#include "post_process_shaders/pp_fsr_rcas_spv.h"
#include "post_process_shaders/pp_fx_bloom_spv.h"
#include "post_process_shaders/pp_fx_blur_spv.h"
#include "post_process_shaders/pp_fx_cartoon_spv.h"
#include "post_process_shaders/pp_fx_cel_spv.h"
#include "post_process_shaders/pp_fx_chroma_spv.h"
#include "post_process_shaders/pp_fx_crt_spv.h"
#include "post_process_shaders/pp_fx_deband_spv.h"
#include "post_process_shaders/pp_fx_denoise_spv.h"
#include "post_process_shaders/pp_fx_filmic_spv.h"
#include "post_process_shaders/pp_fx_grade_spv.h"
#include "post_process_shaders/pp_fx_grain_spv.h"
#include "post_process_shaders/pp_fx_lens_spv.h"
#include "post_process_shaders/pp_fx_levels_spv.h"
#include "post_process_shaders/pp_fx_mono_spv.h"
#include "post_process_shaders/pp_fx_pixelate_spv.h"
#include "post_process_shaders/pp_fx_scanlines_spv.h"
#include "post_process_shaders/pp_fx_sharpen_spv.h"
#include "post_process_shaders/pp_fx_split_spv.h"
#include "post_process_shaders/pp_fx_vignette_spv.h"
#include "post_process_shaders/pp_sgsr1_spv.h"
#include "post_process_shaders/pp_smaa_blend_spv.h"
#include "post_process_shaders/pp_smaa_edges_spv.h"
#include "post_process_shaders/pp_smaa_weights_spv.h"

// SMAA lookup textures (3rdparty/smaa, MIT).
#include "AreaTex.h"
#include "SearchTex.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Libs::Graphics {

namespace {

constexpr vk::Format COLOR      = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format COLOR_SRGB = vk::Format::eR8G8B8A8Srgb;
constexpr uint32_t   PUSH_SIZE  = 64;
constexpr uint32_t   MAX_EFFECTS = 16;

struct EffectInfo {
	const char*               id;
	std::span<const uint32_t> code;
	std::array<float, 8>      defaults;
};

// The effect ids and parameter orders are shared with the Android settings (PostEffects.kt).
const std::array EFFECTS {
    EffectInfo {"sharpen", PP_FX_SHARPEN_SPV, {0.5f}},
    EffectInfo {"bloom", PP_FX_BLOOM_SPV, {0.6f, 1.5f, 0.4f}},
    EffectInfo {"blur", PP_FX_BLUR_SPV, {2.0f, 1.0f, 0.0f, 0.4f}},
    EffectInfo {"crt", PP_FX_CRT_SPV, {0.5f, 0.3f, 0.08f, 480.0f, 1.0f, 0.0f}},
    EffectInfo {"scanlines", PP_FX_SCANLINES_SPV, {0.35f, 540.0f, 0.5f}},
    EffectInfo {"vignette", PP_FX_VIGNETTE_SPV, {0.5f, 0.8f, 0.45f}},
    EffectInfo {"chroma", PP_FX_CHROMA_SPV, {1.5f, 1.5f}},
    EffectInfo {"grain", PP_FX_GRAIN_SPV, {0.04f, 1.5f, 0.0f}},
    EffectInfo {"grade", PP_FX_GRADE_SPV, {0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f}},
    EffectInfo {"filmic", PP_FX_FILMIC_SPV, {0.0f, 0.4f, 0.5f, 1.0f}},
    EffectInfo {"levels", PP_FX_LEVELS_SPV, {0.0f, 1.0f, 1.0f, 0.0f, 1.0f}},
    EffectInfo {"split", PP_FX_SPLIT_SPV, {210.0f, 0.2f, 40.0f, 0.2f, 0.0f}},
    EffectInfo {"deband", PP_FX_DEBAND_SPV, {0.012f, 12.0f, 0.5f}},
    EffectInfo {"denoise", PP_FX_DENOISE_SPV, {0.25f, 1.0f}},
    EffectInfo {"cartoon", PP_FX_CARTOON_SPV, {1.0f, 0.2f, 6.0f, 1.15f}},
    EffectInfo {"cel", PP_FX_CEL_SPV, {4.0f, 0.2f, 0.8f, 0.25f, 1.2f}},
    EffectInfo {"lens", PP_FX_LENS_SPV, {0.15f, 1.0f}},
    EffectInfo {"pixelate", PP_FX_PIXELATE_SPV, {4.0f}},
    EffectInfo {"mono", PP_FX_MONO_SPV, {1.0f, 0.0f}},
};

[[nodiscard]] bool IsSrgb(vk::Format format) {
	return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb;
}

[[nodiscard]] uint32_t Bits(float value) {
	return std::bit_cast<uint32_t>(value);
}

[[nodiscard]] vk::Extent2D ExtentOf(const VulkanImage& image) {
	return {image.extent.width, image.extent.height};
}

[[nodiscard]] bool HasFeatures(const GraphicContext& graphics, vk::Format format,
                               vk::FormatFeatureFlags features) {
	return (graphics.GetFormatProperties(format).optimalTilingFeatures & features) == features;
}

} // namespace

PostProcessor::PostProcessor(GraphicContext& graphics)
    : m_graphics(graphics), m_start(std::chrono::steady_clock::now()) {
	using F = vk::FormatFeatureFlagBits;
	m_supported =
	    HasFeatures(graphics, COLOR,
	                F::eColorAttachment | F::eSampledImageFilterLinear | F::eBlitSrc | F::eBlitDst) &&
	    HasFeatures(graphics, COLOR_SRGB, F::eBlitSrc | F::eBlitDst) &&
	    HasFeatures(graphics, vk::Format::eR8G8Unorm,
	                F::eColorAttachment | F::eSampledImageFilterLinear | F::eTransferDst) &&
	    HasFeatures(graphics, vk::Format::eR8Unorm, F::eSampledImage | F::eTransferDst) &&
	    graphics.max_push_descriptors >= 3;
	if (!m_supported) {
		return;
	}
	m_fp16 = graphics.shader_float16_int16_enabled;

	vk::SamplerCreateInfo sampler {};
	sampler.magFilter    = vk::Filter::eLinear;
	sampler.minFilter    = vk::Filter::eLinear;
	sampler.mipmapMode   = vk::SamplerMipmapMode::eNearest;
	sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	sampler.maxLod       = 0.0f;
	RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &m_linear),
	                     "create post-processing sampler");
	sampler.magFilter = vk::Filter::eNearest;
	sampler.minFilter = vk::Filter::eNearest;
	RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &m_nearest),
	                     "create post-processing sampler");
	m_vertex = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, graphics.device);

	m_bicubic.code = PP_BICUBIC_SPV;
	m_easu.code    = m_fp16 ? std::span<const uint32_t>(PP_FSR_EASU_FP16_SPV) : PP_FSR_EASU_SPV;
	m_rcas.code    = m_fp16 ? std::span<const uint32_t>(PP_FSR_RCAS_FP16_SPV) : PP_FSR_RCAS_SPV;
	m_sgsr.code    = PP_SGSR1_SPV;
	m_smaa_edges.code          = PP_SMAA_EDGES_SPV;
	m_smaa_edges.format        = vk::Format::eR8G8Unorm;
	m_smaa_weights.code        = PP_SMAA_WEIGHTS_SPV;
	m_smaa_weights.inputs      = 3;
	m_smaa_blend.code          = PP_SMAA_BLEND_SPV;
	m_smaa_blend.inputs        = 2;
	m_effect_passes.resize(EFFECTS.size());
	for (size_t i = 0; i < EFFECTS.size(); i++) {
		m_effect_passes[i].code = EFFECTS[i].code;
	}
}

PostProcessor::~PostProcessor() {
	if (!m_supported) {
		return;
	}
	{
		Common::LockGuard queue_lock(m_graphics.queue_mutex);
		(void)m_graphics.queue.waitIdle();
	}
	for (auto* pass: {&m_bicubic, &m_easu, &m_rcas, &m_sgsr, &m_smaa_edges, &m_smaa_weights,
	                  &m_smaa_blend}) {
		DestroyPass(*pass);
	}
	for (auto& pass: m_effect_passes) {
		DestroyPass(pass);
	}
	for (auto* target: {&m_input, &m_edges, &m_weights, &m_antialiased, &m_upscaled, &m_sharpened,
	                    &m_area, &m_search, &m_chain[0], &m_chain[1]}) {
		DestroyTarget(*target);
	}
	if (m_staging != VK_NULL_HANDLE) {
		vmaDestroyBuffer(m_graphics.allocator, m_staging, m_staging_allocation);
	}
	m_graphics.device.destroyShaderModule(m_vertex, nullptr);
	m_graphics.device.destroySampler(m_linear, nullptr);
	m_graphics.device.destroySampler(m_nearest, nullptr);
}

void PostProcessor::SetEffects(std::string_view chain) {
	m_effects.clear();
	while (!chain.empty() && m_effects.size() < MAX_EFFECTS) {
		const auto end   = chain.find(';');
		auto       entry = chain.substr(0, end);
		chain.remove_prefix(end == std::string_view::npos ? chain.size() : end + 1);
		if (entry.empty() || entry.front() == '!') {
			continue;
		}
		const auto colon = entry.find(':');
		const auto id    = entry.substr(0, colon);
		const auto info  = std::find_if(EFFECTS.begin(), EFFECTS.end(),
		                                [id](const EffectInfo& effect) { return id == effect.id; });
		if (info == EFFECTS.end()) {
			continue;
		}
		Effect effect {static_cast<uint32_t>(info - EFFECTS.begin()), info->defaults};
		if (colon != std::string_view::npos) {
			const std::string values(entry.substr(colon + 1));
			const char*       cursor = values.c_str();
			for (auto& param: effect.params) {
				char*       end_of_value = nullptr;
				const float value        = std::strtof(cursor, &end_of_value);
				if (end_of_value == cursor) {
					break;
				}
				if (std::isfinite(value)) {
					param = value;
				}
				cursor = end_of_value;
				if (*cursor != ',') {
					break;
				}
				cursor++;
			}
		}
		m_effects.push_back(effect);
	}
}

void PostProcessor::CreatePipeline(Pass& pass) {
	auto& device = m_graphics.device;
	std::array<vk::DescriptorSetLayoutBinding, 4> bindings {};
	EXIT_IF(pass.inputs > bindings.size());
	for (uint32_t i = 0; i < pass.inputs; i++) {
		bindings[i] = {i, vk::DescriptorType::eCombinedImageSampler, 1,
		               vk::ShaderStageFlagBits::eFragment};
	}
	vk::DescriptorSetLayoutCreateInfo set_layout {};
	set_layout.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	set_layout.bindingCount = pass.inputs;
	set_layout.pBindings    = bindings.data();
	RequireVulkanSuccess(device.createDescriptorSetLayout(&set_layout, nullptr, &pass.set_layout),
	                     "create post-processing descriptor layout");
	const vk::PushConstantRange  range {vk::ShaderStageFlagBits::eFragment, 0, PUSH_SIZE};
	vk::PipelineLayoutCreateInfo layout {};
	layout.setLayoutCount         = 1;
	layout.pSetLayouts            = &pass.set_layout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges    = &range;
	RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &pass.layout),
	                     "create post-processing pipeline layout");

	const auto fragment = CompileSPV(pass.code, device);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = fragment;
	stages[1].pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.attachmentCount = 1;
	blend.pAttachments    = &attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &pass.format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend;
	create.pDynamicState       = &dynamic;
	create.layout              = pass.layout;
	const auto result = device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pass.pipeline);
	device.destroyShaderModule(fragment, nullptr);
	RequireVulkanSuccess(result, "create post-processing pipeline");
}

void PostProcessor::DestroyPass(Pass& pass) {
	m_graphics.device.destroyPipeline(pass.pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(pass.layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(pass.set_layout, nullptr);
	pass.pipeline   = nullptr;
	pass.layout     = nullptr;
	pass.set_layout = nullptr;
}

void PostProcessor::EnsureTarget(Target& target, vk::Extent2D extent, vk::Format format,
                                 vk::ImageUsageFlags usage) {
	if (target.image.image != nullptr && ExtentOf(target.image) == extent &&
	    target.image.format == format) {
		return;
	}
	if (target.image.image != nullptr) {
		// Earlier presentations may still read it.
		{
			Common::LockGuard queue_lock(m_graphics.queue_mutex);
			(void)m_graphics.queue.waitIdle();
		}
		if (&target.image == m_enhanced) {
			m_enhanced = nullptr;
		}
		DestroyTarget(target);
	}
	const bool          srgb = format == COLOR_SRGB;
	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = usage;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (srgb) {
		create.flags = vk::ImageCreateFlagBits::eMutableFormat;
	}
	if (!m_graphics.CreateImage(create, target.image)) {
		EXIT("failed to allocate a post-processing image %ux%u format=%d\n", extent.width,
		     extent.height, static_cast<int>(format));
	}
	vk::ImageViewCreateInfo view {};
	view.image            = target.image.image;
	view.viewType         = vk::ImageViewType::e2D;
	view.format           = srgb ? COLOR : format;
	view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	RequireVulkanSuccess(m_graphics.device.createImageView(&view, nullptr, &target.view),
	                     "create post-processing image view");
}

void PostProcessor::DestroyTarget(Target& target) {
	if (target.image.image == nullptr) {
		return;
	}
	m_graphics.device.destroyImageView(target.view, nullptr);
	target.view = nullptr;
	m_graphics.DeleteImage(target.image);
}

void PostProcessor::Use(vk::CommandBuffer command, VulkanImage& image, vk::ImageLayout layout,
                        vk::PipelineStageFlags2 stage, vk::AccessFlags2 access, bool discard) {
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite |
	                        vk::AccessFlagBits2::eColorAttachmentWrite |
	                        vk::AccessFlagBits2::eMemoryWrite;
	if (!discard && image.state.layout == layout && image.state.access_mask == access &&
	    image.state.pl_stage == stage && !static_cast<bool>(access & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = image.state.pl_stage;
	barrier.srcAccessMask       = image.state.access_mask;
	barrier.dstStageMask        = stage;
	barrier.dstAccessMask       = access;
	barrier.oldLayout           = discard ? vk::ImageLayout::eUndefined : image.state.layout;
	barrier.newLayout           = layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image.image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

// `source` is in TransferSrcOptimal.
void PostProcessor::Blit(vk::CommandBuffer command, const VulkanImage& source, Target& target,
                         vk::Filter filter) {
	Use(command, target.image, vk::ImageLayout::eTransferDstOptimal,
	    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, true);
	vk::ImageBlit region {};
	region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(source.extent.width),
	                                     static_cast<int32_t>(source.extent.height), 1};
	region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(target.image.extent.width),
	                                     static_cast<int32_t>(target.image.extent.height), 1};
	command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, target.image.image,
	                  vk::ImageLayout::eTransferDstOptimal, 1, &region, filter);
}

void PostProcessor::Draw(vk::CommandBuffer command, Pass& pass, std::initializer_list<Input> inputs,
                         Target& target, const void* push, uint32_t push_size, bool clear) {
	EXIT_IF(inputs.size() != pass.inputs || push_size > PUSH_SIZE);
	if (pass.pipeline == nullptr) {
		CreatePipeline(pass);
	}
	std::array<vk::DescriptorImageInfo, 4> infos {};
	std::array<vk::WriteDescriptorSet, 4>  writes {};
	uint32_t                               count = 0;
	for (const auto& input: inputs) {
		Use(command, input.target->image, vk::ImageLayout::eShaderReadOnlyOptimal,
		    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
		infos[count]                  = {input.sampler, input.target->view,
		                                 vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[count].dstBinding      = count;
		writes[count].descriptorCount = 1;
		writes[count].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		writes[count].pImageInfo      = &infos[count];
		count++;
	}
	Use(command, target.image, vk::ImageLayout::eColorAttachmentOptimal,
	    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
	    vk::AccessFlagBits2::eColorAttachmentWrite, true);

	const vk::Extent2D          extent = ExtentOf(target.image);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = target.view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	attachment.clearValue  = vk::ClearValue {vk::ClearColorValue {}};
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, pass.pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pass.layout, 0, count,
	                             writes.data());
	command.pushConstants(pass.layout, vk::ShaderStageFlagBits::eFragment, 0, push_size, push);
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(extent.width),
	                             static_cast<float>(extent.height), 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void PostProcessor::UploadSmaaTextures(vk::CommandBuffer command) {
	constexpr auto lookup = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
	EnsureTarget(m_area, {AREATEX_WIDTH, AREATEX_HEIGHT}, vk::Format::eR8G8Unorm, lookup);
	EnsureTarget(m_search, {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}, vk::Format::eR8Unorm, lookup);

	vk::BufferCreateInfo buffer {};
	buffer.size  = AREATEX_SIZE + SEARCHTEX_SIZE;
	buffer.usage = vk::BufferUsageFlagBits::eTransferSrc;
	VmaAllocationCreateInfo allocation {};
	allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	allocation.flags =
	    VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo mapped {};
	RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(
	                         m_graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer),
	                         &allocation, &m_staging, &m_staging_allocation, &mapped)),
	                     "create SMAA texture upload buffer");
	auto* bytes = static_cast<uint8_t*>(mapped.pMappedData);
	std::memcpy(bytes, areaTexBytes, AREATEX_SIZE);
	std::memcpy(bytes + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
	RequireVulkanSuccess(static_cast<vk::Result>(vmaFlushAllocation(
	                         m_graphics.allocator, m_staging_allocation, 0, buffer.size)),
	                     "flush SMAA texture upload");

	for (auto* target: {&m_area, &m_search}) {
		Use(command, target->image, vk::ImageLayout::eTransferDstOptimal,
		    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, true);
		vk::BufferImageCopy copy {};
		copy.bufferOffset     = target == &m_area ? 0 : AREATEX_SIZE;
		copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.imageExtent      = target->image.extent;
		command.copyBufferToImage(m_staging, target->image.image,
		                          vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	}
	m_smaa_uploaded = true;
}

// SMAA 1x on m_input into m_antialiased, at render resolution.
void PostProcessor::RunSmaa(vk::CommandBuffer command, vk::Format format) {
	if (!m_smaa_uploaded) {
		UploadSmaaTextures(command);
	}
	const vk::Extent2D extent = ExtentOf(m_input.image);
	EnsureTarget(m_edges, extent, vk::Format::eR8G8Unorm);
	EnsureTarget(m_weights, extent, COLOR);
	EnsureTarget(m_antialiased, extent, format);
	const std::array<float, 4> metrics {
	    1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height),
	    static_cast<float>(extent.width), static_cast<float>(extent.height)};
	Draw(command, m_smaa_edges, {{&m_input, m_linear}}, m_edges, metrics.data(), sizeof(metrics),
	     true);
	Draw(command, m_smaa_weights, {{&m_edges, m_linear}, {&m_area, m_linear}, {&m_search, m_nearest}},
	     m_weights, metrics.data(), sizeof(metrics), true);
	Draw(command, m_smaa_blend, {{&m_input, m_linear}, {&m_weights, m_linear}}, m_antialiased,
	     metrics.data(), sizeof(metrics));
}

VulkanImage& PostProcessor::Enhance(vk::CommandBuffer command, const VulkanImage& source,
                                    vk::Extent2D extent, uint64_t serial) {
	EXIT_IF(!m_supported);
	if (m_enhanced != nullptr && serial == m_enhanced_serial && extent == m_enhanced_extent &&
	    m_quality == m_enhanced_quality) {
		return *m_enhanced;
	}
	const auto         format = IsSrgb(source.format) ? COLOR_SRGB : COLOR;
	const vk::Extent2D input  = ExtentOf(source);
	EnsureTarget(m_input, input, format);
	Blit(command, source, m_input, vk::Filter::eNearest);
	Target* current = &m_input;

	if (m_quality.aa == AntiAliasing::Smaa) {
		RunSmaa(command, format);
		current = &m_antialiased;
	}

	if (extent != input) {
		EnsureTarget(m_upscaled, extent, format);
		// The filters made for upscaling fall back to the blit when the frame is not smaller.
		const bool upscaling = extent.width >= input.width && extent.height >= input.height;
		const auto upscaler =
		    upscaling || m_quality.upscaler == Upscaler::Nearest ? m_quality.upscaler : Upscaler::Default;
		const auto in_w = static_cast<float>(input.width);
		const auto in_h = static_cast<float>(input.height);
		const std::array<float, 4> metrics {1.0f / in_w, 1.0f / in_h, in_w, in_h};
		switch (upscaler) {
			case Upscaler::Nearest:
				Use(command, current->image, vk::ImageLayout::eTransferSrcOptimal,
				    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
				Blit(command, current->image, m_upscaled, vk::Filter::eNearest);
				break;
			case Upscaler::Bicubic:
				Draw(command, m_bicubic, {{current, m_linear}}, m_upscaled, metrics.data(),
				     sizeof(metrics));
				break;
			case Upscaler::Sgsr1:
				Draw(command, m_sgsr, {{current, m_linear}}, m_upscaled, metrics.data(),
				     sizeof(metrics));
				break;
			case Upscaler::Fsr1: {
				// FsrEasuCon(): the input fills its image, which is scaled to the whole output.
				const auto out_w = static_cast<float>(extent.width);
				const auto out_h = static_cast<float>(extent.height);
				const std::array<uint32_t, 16> con {
				    Bits(in_w / out_w), Bits(in_h / out_h), Bits(0.5f * in_w / out_w - 0.5f),
				    Bits(0.5f * in_h / out_h - 0.5f), Bits(1.0f / in_w), Bits(1.0f / in_h),
				    Bits(1.0f / in_w), Bits(-1.0f / in_h), Bits(-1.0f / in_w), Bits(2.0f / in_h),
				    Bits(1.0f / in_w), Bits(2.0f / in_h), Bits(0.0f), Bits(4.0f / in_h), 0, 0};
				Draw(command, m_easu, {{current, m_linear}}, m_upscaled, con.data(), sizeof(con));
				break;
			}
			case Upscaler::Default:
			default:
				Use(command, current->image, vk::ImageLayout::eTransferSrcOptimal,
				    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
				Blit(command, current->image, m_upscaled, vk::Filter::eLinear);
				break;
		}
		current = &m_upscaled;
	}

	if (m_quality.sharpen != 0) {
		EnsureTarget(m_sharpened, extent, format);
		// 100: the strongest (0 stops), 50: one stop softer.
		const float stops =
		    2.0f * (1.0f - static_cast<float>(std::min(m_quality.sharpen, 100u)) / 100.0f);
		Draw(command, m_rcas, {{current, m_nearest}}, m_sharpened, &stops, sizeof(stops));
		current = &m_sharpened;
	}

	Use(command, current->image, vk::ImageLayout::eTransferSrcOptimal,
	    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
	m_enhanced         = &current->image;
	m_enhanced_serial  = serial;
	m_enhanced_extent  = extent;
	m_enhanced_quality = m_quality;
	return current->image;
}

VulkanImage& PostProcessor::ApplyEffects(vk::CommandBuffer command, const VulkanImage& source,
                                         vk::Extent2D extent) {
	EXIT_IF(!m_supported || m_effects.empty());
	const auto format = IsSrgb(source.format) ? COLOR_SRGB : COLOR;
	EnsureTarget(m_chain[0], extent, format);
	EnsureTarget(m_chain[1], extent, format);
	// The scaling the presentation blit would do.
	Blit(command, source, m_chain[0], vk::Filter::eLinear);

	struct Push {
		std::array<float, 4> size;
		std::array<float, 4> time;
		std::array<float, 8> params;
	} push {};
	static_assert(sizeof(Push) == PUSH_SIZE);
	const auto width  = static_cast<float>(extent.width);
	const auto height = static_cast<float>(extent.height);
	push.size         = {width, height, 1.0f / width, 1.0f / height};
	const auto seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	push.time = {static_cast<float>(std::fmod(seconds, 3600.0)),
	             static_cast<float>(m_presented % 65536u), 0.0f, 0.0f};
	m_presented++;

	uint32_t current = 0;
	for (const auto& effect: m_effects) {
		push.params = effect.params;
		Draw(command, m_effect_passes[effect.index], {{&m_chain[current], m_linear}},
		     m_chain[1 - current], &push, sizeof(push));
		current = 1 - current;
	}
	Use(command, m_chain[current].image, vk::ImageLayout::eTransferSrcOptimal,
	    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
	return m_chain[current].image;
}

} // namespace Libs::Graphics
