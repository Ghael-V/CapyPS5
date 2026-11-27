// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Contrast-adaptive sharpening: a cross-shaped high-pass whose weight shrinks where the local
// contrast is already high, so edges get crisper without halos. p0.x: amount (0-1).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3 e = Fetch(uv);
	const vec3 b = FetchOffset(uv, vec2(0.0, -1.0));
	const vec3 d = FetchOffset(uv, vec2(-1.0, 0.0));
	const vec3 f = FetchOffset(uv, vec2(1.0, 0.0));
	const vec3 h = FetchOffset(uv, vec2(0.0, 1.0));
	const vec3 a = FetchOffset(uv, vec2(-1.0, -1.0));
	const vec3 c = FetchOffset(uv, vec2(1.0, -1.0));
	const vec3 g = FetchOffset(uv, vec2(-1.0, 1.0));
	const vec3 i = FetchOffset(uv, vec2(1.0, 1.0));
	// Soft minimum and maximum, 0-2: the cross, plus the whole 3x3 block.
	const vec3 cross_min = min(min(min(b, d), min(f, h)), e);
	const vec3 cross_max = max(max(max(b, d), max(f, h)), e);
	const vec3 low  = cross_min + min(cross_min, min(min(a, c), min(g, i)));
	const vec3 high = cross_max + max(cross_max, max(max(a, c), max(g, i)));
	// Headroom before clipping, relative to the local peak.
	const vec3 amplitude = sqrt(clamp(min(low, 2.0 - high) / max(high, 1.0 / 64.0), 0.0, 1.0));
	const float peak = -1.0 / mix(8.0, 4.5, clamp(p0.x, 0.0, 1.0));
	const vec3 w = amplitude * peak;
	Output(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));
}
