// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Monochrome: grayscale, or sepia toning. p0: amount, sepia (0: grayscale).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const vec3 color = Fetch(uv);
	const vec3 gray  = vec3(Luma(color));
	const vec3 sepia = vec3(dot(color, vec3(0.393, 0.769, 0.189)), dot(color, vec3(0.349, 0.686, 0.168)),
	                        dot(color, vec3(0.272, 0.534, 0.131)));
	Output(mix(color, mix(gray, sepia, clamp(p0.y, 0.0, 1.0)), clamp(p0.x, 0.0, 1.0)));
}
