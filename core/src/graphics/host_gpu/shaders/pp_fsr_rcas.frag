// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// AMD FidelityFX Super Resolution 1 RCAS sharpening (3rdparty/fidelityfx-fsr, MIT) as a fullscreen
// pass at output resolution. Built with and without PP_FP16, as pp_fsr_easu.frag.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform Push {
	// Sharpness in stops: 0 is the strongest, each stop halves it.
	float stops;
};

#define A_GPU  1
#define A_GLSL 1
#if defined(PP_FP16)
#define A_HALF 1
#endif
#include "ffx_a.h"

#if defined(PP_FP16)
#define FSR_RCAS_H 1
AH4  FsrRcasLoadH(ASW2 p) { return AH4(texelFetch(source, ASU2(p), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
AF4  FsrRcasLoadF(ASU2 p) { return texelFetch(source, p, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#include "ffx_fsr1.h"

void main() {
	AU4 con;
	FsrRcasCon(con, stops);
	const AU2 position = AU2(gl_FragCoord.xy);
#if defined(PP_FP16)
	AH3 color;
	FsrRcasH(color.r, color.g, color.b, position, con);
#else
	AF3 color;
	FsrRcasF(color.r, color.g, color.b, position, con);
#endif
	out_color = vec4(vec3(color), 1.0);
}
