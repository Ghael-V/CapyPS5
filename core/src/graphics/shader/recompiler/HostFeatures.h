#pragma once

namespace Libs::Graphics::ShaderRecompiler {

// Optional Vulkan features of the selected device that change the emitted SPIR-V. They are set
// once at device creation, before any shader is compiled.
struct HostFeatures {
	bool clip_distance = true;
	bool cull_distance = true;
	bool output_layer = true;
	bool output_viewport_index = true;
	bool buffer_int64_atomics = true;
};

inline HostFeatures g_host_features;

} // namespace Libs::Graphics::ShaderRecompiler
