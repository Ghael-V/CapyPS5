// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Film grain: animated noise, strongest in the mid-tones. p0: intensity, grain size (pixels at
// 1080p), colour (0: monochrome grain).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3       color = Fetch(uv);
	const highp vec2 cell  = floor(gl_FragCoord.xy / max(p0.y * PixelScale(), 1.0));
	const highp float frame = mod(time.y, 4096.0);
	const highp vec2 seed  = cell + vec2(frame * 17.0, frame * 31.0);
	const float mono    = float(Hash(seed)) - 0.5;
	const vec3  tint    = vec3(Hash(seed + 7.13), Hash(seed + 3.71), Hash(seed + 5.37)) - 0.5;
	const vec3  noise   = mix(vec3(mono), tint, clamp(p0.z, 0.0, 1.0));
	const float midtone = 1.0 - abs(Luma(color) * 2.0 - 1.0) * 0.6;
	Output(color + noise * (p0.x * 2.0 * midtone));
}
