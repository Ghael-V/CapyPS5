// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Cel shading: the lighting (luma) collapsed into flat bands with soft steps, hues kept, ink
// outlines. p0: bands, band softness, outline strength, outline threshold; p1.x: saturation.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

#include "pp_fx_outline.glsl"

void main() {
	vec3        color  = Fetch(uv);
	const float luma   = Luma(color);
	const float bands  = max(p0.x, 2.0);
	const float scaled = luma * bands;
	const float soft   = clamp(p0.y, 0.0, 1.0) * 0.5;
	const float banded = (floor(scaled) + smoothstep(0.5 - soft, 0.5 + soft, fract(scaled))) / bands;
	color *= banded / max(luma, 1.0 / 255.0);
	color = mix(vec3(Luma(color)), color, p1.x);
	Output(color * (1.0 - clamp(Outline(uv, p0.w) * p0.z, 0.0, 1.0)));
}
