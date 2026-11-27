// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Vignette: darker towards the corners. p0: strength, radius (1: the corners), softness.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3        color   = Fetch(uv);
	const highp float aspect  = size.x * size.w;
	const highp vec2  centred = (uv - 0.5) * vec2(aspect, 1.0);
	// 1 at the corners.
	const float distance = float(length(centred) / length(vec2(aspect, 1.0) * 0.5));
	const float softness = max(p0.z, 0.01);
	const float shade    = smoothstep(p0.y - softness, p0.y + softness * 0.5, distance);
	Output(color * max(1.0 - p0.x * shade, 0.0));
}
