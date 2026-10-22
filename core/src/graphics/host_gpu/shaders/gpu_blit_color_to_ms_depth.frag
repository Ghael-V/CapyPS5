// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout(binding = 0, set = 0) uniform texture2D color;
layout(location = 0) in vec2 uv;

void main() {
	ivec2 coord  = ivec2(uv * vec2(textureSize(color, 0)));
	gl_FragDepth = texelFetch(color, coord, 0)[gl_SampleID];
}
