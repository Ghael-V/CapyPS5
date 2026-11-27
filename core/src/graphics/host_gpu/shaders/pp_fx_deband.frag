// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Debanding: where the neighbourhood is nearly flat, the pixel becomes the mean of four random
// taps around it, then dithered. p0: threshold, radius (pixels at 1080p), dither.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3        color  = Fetch(uv);
	const highp float noise  = Hash(gl_FragCoord.xy + mod(time.y, 64.0));
	const highp float angle  = noise * 6.2831853;
	const highp float radius = (0.5 + 0.5 * Hash(gl_FragCoord.yx)) * p0.y * PixelScale();
	const highp vec2  a = vec2(cos(angle), sin(angle)) * radius;
	const highp vec2  b = vec2(-a.y, a.x);
	const vec3 t0 = FetchOffset(uv, a);
	const vec3 t1 = FetchOffset(uv, -a);
	const vec3 t2 = FetchOffset(uv, b);
	const vec3 t3 = FetchOffset(uv, -b);
	const vec3 spread = max(max(abs(t0 - color), abs(t1 - color)), max(abs(t2 - color), abs(t3 - color)));
	const bool flat_area = max(max(spread.r, spread.g), spread.b) < p0.x;
	vec3 result = flat_area ? (t0 + t1 + t2 + t3) * 0.25 : color;
	result += (float(noise) - 0.5) * (p0.z / 255.0);
	Output(result);
}
