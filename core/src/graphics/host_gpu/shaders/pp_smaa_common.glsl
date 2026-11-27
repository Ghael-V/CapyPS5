// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// SMAA 1x (3rdparty/smaa, MIT) as three fullscreen passes at render resolution. The vertex-stage
// offsets of SMAA are computed per fragment: every pass draws one fullscreen triangle.

layout(location = 0) in vec2 uv;
layout(push_constant) uniform Push {
	// 1/width, 1/height, width, height of the render-resolution image.
	vec4 metrics;
};

#define SMAA_RT_METRICS metrics
#define SMAA_GLSL_4     1
#define SMAA_PRESET_HIGH 1
#include "SMAA.hlsl"
