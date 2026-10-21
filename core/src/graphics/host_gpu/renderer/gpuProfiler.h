// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// `setprop debug.capy.gpu_profile 1`: GPU timestamps around every render pass and compute
// dispatch, summed per render target / shader and logged every 2 s (tag CapyPS5-GpuProf).
// `gpu_profile 2` also logs every pass as it closes (tag CapyPS5-GpuPass), in order.
// Runs on the GPU thread only.
namespace GpuProfiler {

[[nodiscard]] bool Enabled();
// Describes the render pass the next BeginScope opens (first draw of the pass).
void SetPassLabel(uint64_t target, uint32_t width, uint32_t height, uint64_t pixel_shader);
void NoteDraw();
// Scopes are recorded outside render passes: before vkCmdBeginRendering, after vkCmdEndRendering.
void BeginPass(GraphicContext& graphics, vk::CommandBuffer command, uint32_t width, uint32_t height,
               uint32_t colors, bool depth);
void BeginDispatch(GraphicContext& graphics, vk::CommandBuffer command, uint64_t compute_shader,
                   uint32_t groups);
void EndScope(vk::CommandBuffer command);
// Records who ended the current render pass (offset in the emulator library), for gpu_profile 2.
void SetEndCaller(const void* caller);
// Reads finished timestamps; call after each queue submission.
void Collect(GraphicContext& graphics);
// Logs the oldest scopes still pending and whether their timestamps were written (tag
// CapyPS5-GpuHang): the hung pass or dispatch has a begin and no end. Any thread.
void ReportHang();

} // namespace GpuProfiler

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
