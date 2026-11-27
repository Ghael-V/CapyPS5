//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

// Snapdragon Game Super Resolution v1, from sgsr1_shader_mobile.frag in this folder (see LICENSE),
// modified for CapyPS5 (Copyright 2026 CapyPS5 Project): Vulkan GLSL 450, the viewport in a push
// constant, the operation mode as a constant (textureGather needs one), RGBA mode only, no precision
// qualifiers on constructors (desktop GLSL rejects them), and the
// fullscreen-triangle texture coordinate of the CapyPS5 post-processing passes. The algorithm and
// its constants are unchanged.

#version 450

precision mediump float;
precision highp int;

#define EdgeThreshold 8.0/255.0
#define EdgeSharpness 2.0

layout(push_constant) uniform Push {
	// 1/width, 1/height, width, height of the input image.
	highp vec4 ViewportInfo;
};
layout(binding = 0) uniform mediump sampler2D ps0;

layout(location = 0) in highp vec2 in_TEXCOORD0;
layout(location = 0) out vec4 out_Target0;

float fastLanczos2(float x)
{
	float wA = x-4.0;
	float wB = x*wA-wA;
	wA *= wA;
	return wB*wA;
}
vec2 weightY(float dx, float dy,float c, float std)
{
	float x = ((dx*dx)+(dy* dy))* 0.55 + clamp(abs(c)*std, 0.0, 1.0);
	float w = fastLanczos2(x);
	return vec2(w, w * c);
}

void main()
{
	const int mode = 1;
	float edgeThreshold = EdgeThreshold;
	float edgeSharpness = EdgeSharpness;

	vec4 color;
	color.xyz = textureLod(ps0,in_TEXCOORD0.xy,0.0).xyz;

	highp vec2 imgCoord = ((in_TEXCOORD0.xy*ViewportInfo.zw)+vec2(-0.5,0.5));
	highp vec2 imgCoordPixel = floor(imgCoord);
	highp vec2 coord = (imgCoordPixel*ViewportInfo.xy);
	vec2 pl = (imgCoord+(-imgCoordPixel));
	vec4  left = textureGather(ps0,coord, mode);

	float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y)  + abs(color[mode] - left.z) ;
	if(edgeVote > edgeThreshold)
	{
		coord.x += ViewportInfo.x;

		vec4 right = textureGather(ps0,coord + vec2(ViewportInfo.x, 0.0), mode);
		vec4 upDown;
		upDown.xy = textureGather(ps0,coord + vec2(0.0, -ViewportInfo.y),mode).wz;
		upDown.zw  = textureGather(ps0,coord+ vec2(0.0, ViewportInfo.y), mode).yx;

		float mean = (left.y+left.z+right.x+right.w)*0.25;
		left = left - vec4(mean);
		right = right - vec4(mean);
		upDown = upDown - vec4(mean);
		color.w =color[mode] - mean;

		float sum = (((((abs(left.x)+abs(left.y))+abs(left.z))+abs(left.w))+(((abs(right.x)+abs(right.y))+abs(right.z))+abs(right.w)))+(((abs(upDown.x)+abs(upDown.y))+abs(upDown.z))+abs(upDown.w)));
		float std = 2.181818/sum;

		vec2 aWY = weightY(pl.x, pl.y+1.0, upDown.x,std);
		aWY += weightY(pl.x-1.0, pl.y+1.0, upDown.y,std);
		aWY += weightY(pl.x-1.0, pl.y-2.0, upDown.z,std);
		aWY += weightY(pl.x, pl.y-2.0, upDown.w,std);
		aWY += weightY(pl.x+1.0, pl.y-1.0, left.x,std);
		aWY += weightY(pl.x, pl.y-1.0, left.y,std);
		aWY += weightY(pl.x, pl.y, left.z,std);
		aWY += weightY(pl.x+1.0, pl.y, left.w,std);
		aWY += weightY(pl.x-1.0, pl.y-1.0, right.x,std);
		aWY += weightY(pl.x-2.0, pl.y-1.0, right.y,std);
		aWY += weightY(pl.x-2.0, pl.y, right.z,std);
		aWY += weightY(pl.x-1.0, pl.y, right.w,std);

		float finalY = aWY.y/aWY.x;

		float maxY = max(max(left.y,left.z),max(right.x,right.w));
		float minY = min(min(left.y,left.z),min(right.x,right.w));
		finalY = clamp(edgeSharpness*finalY, minY, maxY);

		float deltaY = finalY -color.w;

		//smooth high contrast input
		deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

		color.x = clamp((color.x+deltaY),0.0,1.0);
		color.y = clamp((color.y+deltaY),0.0,1.0);
		color.z = clamp((color.z+deltaY),0.0,1.0);
	}

	color.w = 1.0;  //assume alpha channel is not used
	out_Target0.xyzw = color;
}
