// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Lens distortion: barrel (> 0) or pincushion (< 0) distortion, with a zoom to fill the screen.
// p0: distortion, zoom.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const highp float aspect   = size.x * size.w;
	const highp vec2  centred  = (uv - 0.5) * vec2(aspect, 1.0);
	const highp float r2       = dot(centred, centred) / (0.25 * (aspect * aspect + 1.0));
	const highp vec2  warped   = centred * (1.0 + p0.x * r2) / max(p0.y, 0.1);
	const highp vec2  position = warped / vec2(aspect, 1.0) + 0.5;
	if (any(lessThan(position, vec2(0.0))) || any(greaterThan(position, vec2(1.0)))) {
		Output(vec3(0.0));
		return;
	}
	Output(Fetch(position));
}
