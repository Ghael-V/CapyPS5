// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Shared interface of the post-processing effects (pp_fx_*.frag): one fullscreen pass each, from
// the previous image of the chain (same size) to the next. Colours are the encoded (display)
// values. Colour maths runs at medium precision (RelaxedPrecision: fp16 on Adreno), positions at
// high precision.

precision mediump float;
precision highp int;

layout(location = 0) in highp vec2 uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform mediump sampler2D source;
layout(push_constant) uniform Push {
	highp vec4 size; // width, height, 1/width, 1/height
	highp vec4 time; // seconds (wrapping), presented frame number, 0, 0
	vec4       p0;   // effect parameters, in the order of the effect table
	vec4       p1;
};

vec3 Fetch(highp vec2 position) {
	return textureLod(source, position, 0.0).rgb;
}

// Texel at an offset in pixels.
vec3 FetchOffset(highp vec2 position, highp vec2 pixels) {
	return textureLod(source, position + pixels * size.zw, 0.0).rgb;
}

float Luma(vec3 color) {
	return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Output pixels per pixel of a 1080-line screen: sizes look the same at any resolution.
highp float PixelScale() {
	return size.y / 1080.0;
}

highp float Hash(highp vec2 p) {
	highp vec3 q = fract(vec3(p.xyx) * 0.1031);
	q += dot(q, q.yzx + 33.33);
	return fract((q.x + q.y) * q.z);
}

void Output(vec3 color) {
	out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
