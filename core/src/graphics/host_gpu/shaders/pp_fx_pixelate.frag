// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Pixelate: big square pixels. p0.x: pixel size (pixels at 1080p).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pp_fx_common.glsl"

void main() {
	const highp float cell     = max(floor(p0.x * PixelScale() + 0.5), 1.0);
	const highp vec2  position = (floor(gl_FragCoord.xy / cell) + 0.5) * cell * size.zw;
	Output(Fetch(position));
}
