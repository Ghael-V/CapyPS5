// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Gaussian blur, optionally keeping a sharp centre (a depth-of-field look).
// p0: radius (pixels at 1080p), strength (0-1), sharp centre size (0: none), centre falloff.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3 color = Fetch(uv);
	float amount = clamp(p0.y, 0.0, 1.0);
	if (p0.z > 0.0) {
		const highp vec2 centred  = (uv - 0.5) * vec2(size.x * size.w, 1.0);
		const float      distance = float(length(centred)) * 2.0;
		amount *= smoothstep(p0.z, p0.z + max(p0.w, 0.01), distance);
	}
	if (amount <= 0.0) {
		Output(color);
		return;
	}
	// 5x5 bilinear taps, spaced by half the radius.
	const highp float spacing = max(p0.x, 0.5) * PixelScale() * 0.5;
	vec3  sum   = vec3(0.0);
	float total = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			const float weight = exp(-0.5 * float(x * x + y * y) / 1.5);
			sum += FetchOffset(uv, vec2(x, y) * spacing) * weight;
			total += weight;
		}
	}
	Output(mix(color, sum / total, amount));
}
