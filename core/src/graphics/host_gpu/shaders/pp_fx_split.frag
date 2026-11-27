// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Split toning: one hue in the shadows, another in the highlights, brightness kept.
// p0: shadow hue (degrees), shadow strength, highlight hue, highlight strength; p1.x: balance.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

vec3 Hue(float degrees) {
	const vec3 rgb = clamp(abs(fract(degrees / 360.0 + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0,
	                       0.0, 1.0);
	return rgb - Luma(rgb);
}

void main() {
	const vec3  color      = Fetch(uv);
	const float luma       = clamp(Luma(color) + p1.x * 0.5, 0.0, 1.0);
	const float shadows    = (1.0 - luma) * (1.0 - luma);
	const float highlights = luma * luma;
	Output(color + Hue(p0.x) * (shadows * p0.y * 0.5) + Hue(p0.z) * (highlights * p0.w * 0.5));
}
