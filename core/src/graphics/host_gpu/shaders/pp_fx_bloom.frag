// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Bloom: the bright parts, spread over a disc of taps around the pixel, added back as a glow.
// p0: threshold (0-1), radius (% of the screen height), amount.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

const int TAPS = 24;

void main() {
	const vec3  color     = Fetch(uv);
	const float threshold = clamp(p0.x, 0.0, 0.99);
	const highp float radius = p0.y * 0.01 * size.y;
	vec3  glow  = vec3(0.0);
	float total = 0.0;
	for (int i = 0; i < TAPS; i++) {
		// Golden-angle spiral: evenly spread taps.
		const highp float t      = (float(i) + 0.5) / float(TAPS);
		const highp float angle  = float(i) * 2.39996323;
		const highp vec2  offset = vec2(cos(angle), sin(angle)) * sqrt(t) * radius;
		const vec3  tap    = FetchOffset(uv, offset);
		const float bright = max(max(tap.r, tap.g), tap.b);
		const float weight = 1.0 - 0.75 * float(t);
		glow += tap * (max(bright - threshold, 0.0) / max(bright, 1.0 / 255.0)) * weight;
		total += weight;
	}
	glow = glow / total * p0.z;
	// Screen blend: highlights saturate towards white instead of clipping.
	Output(1.0 - (1.0 - color) * (1.0 - min(glow, 1.0)));
}
