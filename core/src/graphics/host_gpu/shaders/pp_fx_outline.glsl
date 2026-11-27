// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Ink outline shared by the cartoon effects: Sobel magnitude of the luma, 0 (no edge) to 1.
float Outline(highp vec2 position, float threshold) {
	const float tl = Luma(FetchOffset(position, vec2(-1.0, -1.0)));
	const float t  = Luma(FetchOffset(position, vec2(0.0, -1.0)));
	const float tr = Luma(FetchOffset(position, vec2(1.0, -1.0)));
	const float l  = Luma(FetchOffset(position, vec2(-1.0, 0.0)));
	const float r  = Luma(FetchOffset(position, vec2(1.0, 0.0)));
	const float bl = Luma(FetchOffset(position, vec2(-1.0, 1.0)));
	const float b  = Luma(FetchOffset(position, vec2(0.0, 1.0)));
	const float br = Luma(FetchOffset(position, vec2(1.0, 1.0)));
	const float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
	const float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
	const float magnitude = sqrt(gx * gx + gy * gy);
	return smoothstep(threshold, threshold * 2.0 + 0.05, magnitude);
}
