// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_

#include <cstdint>

namespace Libs::Graphics {

namespace HW {
struct BlendControl;
}
namespace Prospero {
struct ColorComponentMapping;
}

enum class BlendMappingSupport {
	Direct,
	SourceAlpha, // Requires logical alpha in the second blend source.
	SourceAlphaOne, // Source alpha for RGB, one for alpha, with a shared destination equation.
	SourceAlphaZero, // Source alpha for RGB, zero for alpha, with a shared destination equation.
	Unsupported,
};

bool                BlendFactorIsDualSource(uint8_t factor);
BlendMappingSupport ClassifyBlendMapping(const HW::BlendControl&                blend,
                                         const Prospero::ColorComponentMapping& mapping);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_
