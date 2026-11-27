// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Levels. p0: input black, input white, gamma, output black; p1.x: output white.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	vec3 color = (Fetch(uv) - p0.x) / max(p0.y - p0.x, 1.0 / 255.0);
	color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / max(p0.z, 0.05)));
	Output(mix(vec3(p0.w), vec3(p1.x), color));
}
