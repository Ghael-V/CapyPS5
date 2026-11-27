// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// SMAA pass 1: luma edge detection into a cleared RG8 target.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec2 out_edges;
layout(binding = 0) uniform sampler2D color_tex;

#include "pp_smaa_common.glsl"

void main() {
	vec4 offset[3];
	SMAAEdgeDetectionVS(uv, offset);
	out_edges = SMAALumaEdgeDetectionPS(uv, offset, color_tex);
}
