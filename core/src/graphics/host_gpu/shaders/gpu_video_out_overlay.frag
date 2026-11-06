// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#version 450
#extension GL_GOOGLE_include_directive : require

#include "gpu_video_out_hdr10_to_sdr.inc"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform AlphaControl {
	// bit 0: premultiplied alpha, 2: PQ (HDR10) source, 3: linear source, 4: opaque
	uint flags;
};

void main() {
	const uint PQ_SOURCE = 4u;
	color = texture(source, uv);
	if ((flags & 16u) != 0u) {
		color.a = 1.0;
	}
	const bool premultiplied = (flags & 1u) != 0u;
	const bool convert       = (flags & PQ_SOURCE) != 0u;
	if (convert && premultiplied) {
		color.rgb = color.a > 0.0 ? color.rgb / color.a : vec3(0.0);
	}
	if (convert) {
		color.rgb = Hdr10ToSdr(color.rgb, (flags & 8u) != 0u);
	}
	if (!premultiplied || convert) {
		color.rgb *= color.a;
	}
}
