// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Scanlines on a flat screen. p0: strength, line count, line thickness (0-1).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3        color = Fetch(uv);
	const highp float phase = fract(uv.y * max(p0.y, 16.0));
	const float distance  = abs(float(phase) - 0.5) * 2.0;
	const float thickness = clamp(p0.z, 0.05, 0.95);
	const float dark      = smoothstep(thickness - 0.15, thickness + 0.15, distance);
	Output(color * (1.0 - clamp(p0.x, 0.0, 1.0) * dark));
}
