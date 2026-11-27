// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Chromatic aberration: red and blue pulled apart towards the edges of the screen, as a cheap
// lens does. p0: strength (pixels at the corners at 1080p), falloff (1: linear).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const highp vec2  centred  = uv - 0.5;
	const highp float distance = length(centred);
	const float       reach    = pow(clamp(float(distance) * 1.41421356, 0.0, 1.0), max(p0.y, 0.1));
	const highp vec2  offset   = (distance > 0.0 ? centred / distance : vec2(0.0)) *
	                          (p0.x * reach * PixelScale()) * size.zw;
	const float red   = textureLod(source, uv + offset, 0.0).r;
	const float green = textureLod(source, uv, 0.0).g;
	const float blue  = textureLod(source, uv - offset, 0.0).b;
	Output(vec3(red, green, blue));
}
