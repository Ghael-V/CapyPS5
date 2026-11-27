// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_POSTPROCESS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_POSTPROCESS_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

/// Upscaler of the presented frame to the screen (`debug.capy.upscaler`).
enum class Upscaler : uint32_t { Default = 0, Nearest = 1, Bicubic = 2, Fsr1 = 3, Sgsr1 = 4 };
/// Anti-aliasing of the presented frame at render resolution (`debug.capy.aa`).
enum class AntiAliasing : uint32_t { Off = 0, Smaa = 1 };

/// Image quality options. All at their default, nothing runs and the frame is presented as before.
struct QualitySettings {
	Upscaler     upscaler = Upscaler::Default;
	uint32_t     sharpen  = 0; // RCAS strength, 0 (off) to 100
	AntiAliasing aa       = AntiAliasing::Off;

	[[nodiscard]] bool Active() const noexcept {
		return upscaler != Upscaler::Default || sharpen != 0 || aa != AntiAliasing::Off;
	}
	bool operator==(const QualitySettings&) const = default;
};

/// Post-processing of presented frames, in two parts that each work on SDR 8-bit frames:
///
/// - Image quality, once per guest frame: SMAA at render resolution, the upscaler to the screen
///   resolution (the bilinear blit of the presentation by default), then RCAS sharpening. Its
///   output is what CapyPS5 Framegen ingests, so generated frames are made from enhanced frames.
/// - The effects chain, on every presented frame (guest or generated, after Framegen): a list of
///   single-pass effects (bloom, CRT, colour grading...) applied in the user's order at screen
///   resolution. Framegen works on the clean image, and every frame shown gets the same look.
///
/// Every pass is a fullscreen fragment pass into a colour attachment (render targets keep their
/// framebuffer compression on Adreno, which storage images may lose). Images hold the encoded
/// values of the frame: sRGB frames go through UNORM views of sRGB images, as in Framegen.
class PostProcessor final {
public:
	explicit PostProcessor(GraphicContext& graphics);
	~PostProcessor();
	KYTY_CLASS_NO_COPY(PostProcessor);

	/// Whether the device can run it (8-bit render targets that can be blitted and filtered).
	[[nodiscard]] bool Supported() const noexcept { return m_supported; }

	void SetQuality(const QualitySettings& quality) noexcept { m_quality = quality; }
	[[nodiscard]] const QualitySettings& Quality() const noexcept { return m_quality; }

	/// The effects chain: `id:p0,p1,...` entries separated by `;`, in order; a `!` before the id
	/// keeps the entry but skips it. Unknown ids are ignored, missing parameters take defaults.
	void SetEffects(std::string_view chain);
	[[nodiscard]] bool HasEffects() const noexcept { return !m_effects.empty(); }

	/// Image quality passes over `source` (in TransferSrcOptimal) to an image of `extent`, left in
	/// TransferSrcOptimal. The same `serial` and settings return the previous result unchanged.
	[[nodiscard]] VulkanImage& Enhance(vk::CommandBuffer command, const VulkanImage& source,
	                                   vk::Extent2D extent, uint64_t serial);

	/// The effects chain over `source` (in TransferSrcOptimal), scaled to `extent`; the result is
	/// left in TransferSrcOptimal.
	[[nodiscard]] VulkanImage& ApplyEffects(vk::CommandBuffer command, const VulkanImage& source,
	                                        vk::Extent2D extent);

private:
	static constexpr vk::ImageUsageFlags RENDER_USAGE =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
	    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;

	struct Target {
		VulkanImage image;
		vk::ImageView view = nullptr; // UNORM view for sRGB images
	};
	struct Pass {
		std::span<const uint32_t> code;
		uint32_t                  inputs     = 1;
		vk::Format                format     = vk::Format::eR8G8B8A8Unorm;
		vk::DescriptorSetLayout   set_layout = nullptr;
		vk::PipelineLayout        layout     = nullptr;
		vk::Pipeline              pipeline   = nullptr;
	};
	struct Input {
		Target*     target;
		vk::Sampler sampler;
	};
	struct Effect {
		uint32_t             index = 0;
		std::array<float, 8> params {};
	};

	void CreatePipeline(Pass& pass);
	void DestroyPass(Pass& pass);
	void EnsureTarget(Target& target, vk::Extent2D extent, vk::Format format,
	                  vk::ImageUsageFlags usage = RENDER_USAGE);
	void DestroyTarget(Target& target);
	void Use(vk::CommandBuffer command, VulkanImage& image, vk::ImageLayout layout,
	         vk::PipelineStageFlags2 stage, vk::AccessFlags2 access, bool discard = false);
	void Blit(vk::CommandBuffer command, const VulkanImage& source, Target& target,
	          vk::Filter filter);
	void Draw(vk::CommandBuffer command, Pass& pass, std::initializer_list<Input> inputs,
	          Target& target, const void* push, uint32_t push_size, bool clear = false);
	void UploadSmaaTextures(vk::CommandBuffer command);
	void RunSmaa(vk::CommandBuffer command, vk::Format format);

	GraphicContext&  m_graphics;
	bool             m_supported      = false;
	bool             m_fp16           = false;
	vk::Sampler      m_linear         = nullptr;
	vk::Sampler      m_nearest        = nullptr;
	vk::ShaderModule m_vertex         = nullptr;
	QualitySettings  m_quality;

	Pass m_bicubic;
	Pass m_easu;
	Pass m_rcas;
	Pass m_sgsr;
	Pass m_smaa_edges;
	Pass m_smaa_weights;
	Pass m_smaa_blend;
	std::vector<Pass> m_effect_passes;

	// Image quality: the frame copied at render resolution, SMAA, upscaled, sharpened.
	Target m_input;
	Target m_edges;
	Target m_weights;
	Target m_antialiased;
	Target m_upscaled;
	Target m_sharpened;
	// SMAA lookup textures and their upload buffer.
	Target        m_area;
	Target        m_search;
	VkBuffer      m_staging            = VK_NULL_HANDLE;
	VmaAllocation m_staging_allocation = nullptr;
	bool          m_smaa_uploaded      = false;

	VulkanImage*    m_enhanced = nullptr;
	uint64_t        m_enhanced_serial = ~0ull;
	vk::Extent2D    m_enhanced_extent {};
	QualitySettings m_enhanced_quality;

	// Effects chain: ping-pong images at screen resolution.
	std::vector<Effect>                   m_effects;
	std::array<Target, 2>                 m_chain;
	uint64_t                              m_presented = 0;
	std::chrono::steady_clock::time_point m_start;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_POSTPROCESS_H_
