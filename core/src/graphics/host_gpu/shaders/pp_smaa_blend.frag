// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// SMAA pass 3: neighborhood blending of the frame with the weights.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D color_tex;
layout(binding = 1) uniform sampler2D blend_tex;

#include "pp_smaa_common.glsl"

void main() {
	vec4 offset;
	SMAANeighborhoodBlendingVS(uv, offset);
	out_color = vec4(SMAANeighborhoodBlendingPS(uv, offset, color_tex, blend_tex).rgb, 1.0);
}
