// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Bicubic (Catmull-Rom) upscaling: the 4x4 kernel folded into 9 bilinear taps, clamped to the
// nearest 2x2 texels so the negative lobes do not ring.

#version 450

precision mediump float;
precision highp int;

layout(location = 0) in highp vec2 uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform mediump sampler2D source;
layout(push_constant) uniform Push {
	// 1/width, 1/height, width, height of the input image.
	highp vec4 metrics;
};

void main() {
	const highp vec2 position = uv * metrics.zw - 0.5;
	const highp vec2 base     = floor(position);
	const vec2       f        = vec2(position - base);
	// Catmull-Rom weights of the four texels on each axis.
	const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	const vec2 w3 = f * f * (-0.5 + 0.5 * f);
	// The two middle texels share one bilinear tap, placed by their relative weights.
	const vec2       w12 = w1 + w2;
	const highp vec2 t0  = (base - 0.5) * metrics.xy;
	const highp vec2 t12 = (base + 0.5 + w2 / w12) * metrics.xy;
	const highp vec2 t3  = (base + 2.5) * metrics.xy;

	vec3 color = vec3(0.0);
	color += textureLod(source, vec2(t0.x, t0.y), 0.0).rgb * (w0.x * w0.y);
	color += textureLod(source, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y);
	color += textureLod(source, vec2(t3.x, t0.y), 0.0).rgb * (w3.x * w0.y);
	color += textureLod(source, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y);
	color += textureLod(source, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y);
	color += textureLod(source, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y);
	color += textureLod(source, vec2(t0.x, t3.y), 0.0).rgb * (w0.x * w3.y);
	color += textureLod(source, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
	color += textureLod(source, vec2(t3.x, t3.y), 0.0).rgb * (w3.x * w3.y);

	// The 2x2 texels around the sample bound the result.
	const highp vec2 near = (base + 1.0) * metrics.xy;
	const vec4       r    = textureGather(source, near, 0);
	const vec4       g    = textureGather(source, near, 1);
	const vec4       b    = textureGather(source, near, 2);
	const vec3 low  = vec3(min(min(r.x, r.y), min(r.z, r.w)), min(min(g.x, g.y), min(g.z, g.w)),
	                       min(min(b.x, b.y), min(b.z, b.w)));
	const vec3 high = vec3(max(max(r.x, r.y), max(r.z, r.w)), max(max(g.x, g.y), max(g.z, g.w)),
	                       max(max(b.x, b.y), max(b.z, b.w)));
	out_color = vec4(clamp(color, low, high), 1.0);
}
