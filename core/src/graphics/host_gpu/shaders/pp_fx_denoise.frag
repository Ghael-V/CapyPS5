// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Denoising: an edge-preserving (bilateral) average of the 5x5 neighbourhood without its
// corners. p0: strength, radius (pixels at 1080p).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3        color   = Fetch(uv);
	const float       sigma   = max(p0.x, 0.01) * 0.25;
	const float       range   = -0.5 / (sigma * sigma);
	const highp float spacing = max(p0.y, 0.5) * PixelScale() * 0.5;
	vec3  sum   = color;
	float total = 1.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if ((x == 0 && y == 0) || (abs(x) == 2 && abs(y) == 2)) {
				continue;
			}
			const vec3  tap    = FetchOffset(uv, vec2(x, y) * spacing);
			const vec3  delta  = tap - color;
			const float weight = exp(dot(delta, delta) * range - 0.25 * float(x * x + y * y));
			sum += tap * weight;
			total += weight;
		}
	}
	Output(sum / total);
}
