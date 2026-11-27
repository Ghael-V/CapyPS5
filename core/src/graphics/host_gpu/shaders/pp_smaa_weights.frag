// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// SMAA pass 2: blending weights from the edges and the precomputed area and search textures.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 out_weights;
layout(binding = 0) uniform sampler2D edges_tex;
layout(binding = 1) uniform sampler2D area_tex;
layout(binding = 2) uniform sampler2D search_tex;

#include "pp_smaa_common.glsl"

void main() {
	vec2 pixcoord;
	vec4 offset[3];
	SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
	out_weights = SMAABlendingWeightCalculationPS(uv, pixcoord, offset, edges_tex, area_tex,
	                                              search_tex, vec4(0.0));
}
