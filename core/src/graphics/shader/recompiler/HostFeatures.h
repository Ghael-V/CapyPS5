// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler {

// Optional Vulkan features of the selected device that change the emitted SPIR-V. They are set
// once at device creation, before any shader is compiled.
struct HostFeatures {
	bool clip_distance = true;
	bool cull_distance = true;
	bool output_layer = true;
	bool output_viewport_index = true;
	bool buffer_int64_atomics = true;
	// 8-bit storage buffer access (VK_KHR_8bit_storage). Adreno 6xx + Turnip does not
	// expose it; when false, 8-bit buffer loads/stores are compiled against the 32-bit
	// buffer alias and the SPIR-V subword emulators handle the shift/mask extraction
	// and the atomic read-modify-write stores.
	bool buffer_u8 = true;
	// gl_BaseVertex/gl_BaseInstance, needed for GPU-side indirect draws.
	bool draw_parameters = false;
	// BaryCoordKHR for pixel inputs read per vertex. Without it those inputs are declared as
	// ordinary interpolated (or flat) inputs, and per-vertex reads get the interpolated value.
	bool fragment_barycentric = true;
	// OpReadClockKHR Device tick in picoseconds (VK_KHR_shader_clock shaderDeviceClock); 0 without
	// it, and S_MEMREALTIME then keeps a constant.
	uint32_t device_clock_period_ps = 0;
};

inline HostFeatures g_host_features;

} // namespace Libs::Graphics::ShaderRecompiler
