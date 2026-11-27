// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// AMD FidelityFX Super Resolution 1 EASU (3rdparty/fidelityfx-fsr, MIT) as a fullscreen pass.
// Built twice: with PP_FP16 (packed half math, needs shaderFloat16 and shaderInt16) and without.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D source;
// FsrEasuCon() constants, computed on the CPU (PostProcessor::Upscale).
layout(push_constant) uniform Push {
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
};

#define A_GPU  1
#define A_GLSL 1
#if defined(PP_FP16)
#define A_HALF 1
#endif
#include "ffx_a.h"

#if defined(PP_FP16)
#define FSR_EASU_H 1
AH4 FsrEasuRH(AF2 p) { return AH4(textureGather(source, p, 0)); }
AH4 FsrEasuGH(AF2 p) { return AH4(textureGather(source, p, 1)); }
AH4 FsrEasuBH(AF2 p) { return AH4(textureGather(source, p, 2)); }
#else
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(source, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source, p, 2); }
#endif
#include "ffx_fsr1.h"

void main() {
	const AU2 position = AU2(gl_FragCoord.xy);
#if defined(PP_FP16)
	AH3 color;
	FsrEasuH(color, position, con0, con1, con2, con3);
#else
	AF3 color;
	FsrEasuF(color, position, con0, con1, con2, con3);
#endif
	out_color = vec4(vec3(color), 1.0);
}
