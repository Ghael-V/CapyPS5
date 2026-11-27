// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Cartoon: posterized colours with ink outlines. p0: outline strength, outline threshold,
// colour levels, saturation.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

#include "pp_fx_outline.glsl"

void main() {
	vec3 color = Fetch(uv);
	color = mix(vec3(Luma(color)), color, p0.w);
	const float levels = max(p0.z, 2.0) - 1.0;
	color = floor(clamp(color, 0.0, 1.0) * levels + 0.5) / levels;
	Output(color * (1.0 - clamp(Outline(uv, p0.y) * p0.x, 0.0, 1.0)));
}
