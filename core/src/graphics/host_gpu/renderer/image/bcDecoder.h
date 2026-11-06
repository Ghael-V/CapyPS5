// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_BCDECODER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_BCDECODER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
struct GraphicContext;

// How a BCn image is stored on a GPU without BCn textures.
struct BcDecodedFormat {
	uint32_t   shader_format = 0; // 1-7, the BC number; 6 (BC6H) is not decoded
	bool       is_signed     = false;
	vk::Format format        = vk::Format::eUndefined;
	uint32_t   texel_bytes   = 0;
};

// The decoded storage of a BCn format, or nothing for other formats.
[[nodiscard]] std::optional<BcDecodedFormat> BcDecodedFormatOf(vk::Format format) noexcept;

// Decodes BCn blocks into texels on the GPU (bcn_decode.comp), for images stored decoded.
class BcDecoder final {
public:
	BcDecoder(GraphicContext& graphics, CommandScheduler& scheduler);
	~BcDecoder();
	KYTY_CLASS_NO_COPY(BcDecoder);

	// Records the decoding of the blocks `copy` reads from `source` (the bytes at
	// [source_offset, source_offset + source_size) hold them all) and returns the copy that writes
	// the texels into the decoded image from `decoded`. Nothing when the format cannot be decoded.
	[[nodiscard]] std::optional<vk::BufferImageCopy>
	Decode(vk::CommandBuffer command, vk::Buffer source, uint64_t source_offset,
	       uint64_t source_size, const BcDecodedFormat& format, const vk::BufferImageCopy& copy,
	       vk::Buffer& decoded);

private:
	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
	std::unique_ptr<Buffer> m_output;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_BCDECODER_H_
