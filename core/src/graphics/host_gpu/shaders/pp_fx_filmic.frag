// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Filmic tone curve: exposure, then an S-curve with separately shaped shadows (toe) and
// highlights (shoulder), highlights rolled off instead of clipped. p0: exposure (stops), toe,
// shoulder, amount.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

vec3 Curve(vec3 x, float toe, float shoulder) {
	const vec3 low  = 0.5 * pow(max(2.0 * x, 0.0), vec3(1.0 + toe));
	const vec3 high = 1.0 - 0.5 * pow(max(2.0 * (1.0 - x), 0.0), vec3(1.0 + shoulder));
	return mix(low, high, step(0.5, x));
}

void main() {
	const vec3 color = Fetch(uv);
	vec3 exposed = color * exp2(p0.x);
	// Soft shoulder above 0.8, so overexposed parts keep some gradation.
	const vec3 over = max(exposed - 0.8, 0.0);
	exposed = min(exposed, 0.8) + 0.2 * (over / (over + 0.2));
	Output(mix(color, Curve(clamp(exposed, 0.0, 1.0), p0.y, p0.z), clamp(p0.w, 0.0, 1.0)));
}
