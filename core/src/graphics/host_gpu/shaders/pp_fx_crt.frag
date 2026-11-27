// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// CRT television: curved glass, colour bleed, scanlines, an aperture-grille mask and rounded
// corners. p0: scanline strength, mask strength, curvature, line count; p1: bleed (pixels at 1080p),
// roll speed (lines per second).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	// Barrel-curved screen.
	const highp vec2 centred = uv - 0.5;
	const highp vec2 curved  = uv + centred * dot(centred, centred) * p0.z * 2.0;
	if (any(lessThan(curved, vec2(0.0))) || any(greaterThan(curved, vec2(1.0)))) {
		Output(vec3(0.0));
		return;
	}
	// Colour bleed: the beam smears along the line.
	const highp float bleed = p1.x * PixelScale();
	vec3 color = Fetch(curved) * 0.5;
	color += FetchOffset(curved, vec2(-bleed, 0.0)) * 0.25;
	color += FetchOffset(curved, vec2(bleed, 0.0)) * 0.25;

	// Scanlines: brightest at the middle of each line, wider for bright pixels.
	const highp float lines = max(p0.w, 16.0);
	const highp float phase = fract(curved.y * lines + time.x * p1.y);
	const float distance = abs(float(phase) - 0.5) * 2.0;
	const float width    = mix(0.65, 1.0, Luma(color));
	const float beam     = 1.0 - smoothstep(0.0, width, distance) * 0.85;
	color *= mix(1.0, beam * 1.25, clamp(p0.x, 0.0, 1.0));

	// Aperture grille: each output column passes one primary more than the others.
	const int  column = int(gl_FragCoord.x) % 3;
	const vec3 mask   = vec3(column == 0 ? 1.0 : 0.6, column == 1 ? 1.0 : 0.6, column == 2 ? 1.0 : 0.6);
	color *= mix(vec3(1.0), mask * 1.2, clamp(p0.y, 0.0, 1.0));

	// Rounded, darkened corners.
	const highp vec2 edge = min(curved, 1.0 - curved) * vec2(size.x * size.w, 1.0);
	color *= smoothstep(0.0, 0.02, float(min(edge.x, edge.y)));
	Output(color);
}
