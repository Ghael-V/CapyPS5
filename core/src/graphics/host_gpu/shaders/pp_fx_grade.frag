// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Colour grading. p0: brightness, contrast, saturation, vibrance; p1: gamma, temperature
// (warmer > 0), tint (magenta > 0).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	vec3 color = Fetch(uv) + p0.x;
	color = (color - 0.5) * p0.y + 0.5;
	const float luma = Luma(color);
	color = mix(vec3(luma), color, p0.z);
	// Vibrance: more saturation for the colours that have little.
	const float chroma = max(max(color.r, color.g), color.b) - min(min(color.r, color.g), color.b);
	color = mix(vec3(luma), color, 1.0 + p0.w * (1.0 - clamp(chroma, 0.0, 1.0)));
	color *= vec3(1.0 + 0.12 * p1.y + 0.05 * p1.z, 1.0 - 0.1 * p1.z, 1.0 - 0.12 * p1.y + 0.05 * p1.z);
	color = pow(max(color, 0.0), vec3(1.0 / max(p1.x, 0.05)));
	Output(color);
}
