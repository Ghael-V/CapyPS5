// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_

#include "graphics/shader/shader.h"

#include <array>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
} // namespace HW

struct ShaderParams {
	std::span<const uint32_t> code;
	std::array<uint32_t, 40>  user_data {}; // 32 user SGPRs plus the merged-stage s0:s7 prefix.
	uint32_t                  user_data_count = 0;
	uint64_t                  hash = 0;
	std::span<const uint32_t> back_code;

	[[nodiscard]] uint64_t Base() const {
		return reinterpret_cast<uint64_t>(code.data());
	}
};

void BuildStageStaticKey(const ShaderVertexInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderPixelInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderComputeInputInfo& input_info, std::vector<uint32_t>& key);

// Whether PrepareProgram turns this vertex stage into a (native or emulated) mesh program.
bool VertexProgramUsesMesh(const HW::VertexShaderInfo& regs, const HW::Context& context,
                           bool mesh_shader_enabled);
ShaderParams PrepareProgram(const HW::VertexShaderInfo& regs, const HW::Context& context,
                            const HW::UserConfig& user_config, ShaderVertexInputInfo& input_info,
                            bool mesh_shader_enabled = true, bool emulate_merged_gs = false);
std::array<ShaderParams, 3>
PrepareTessellationPrograms(const HW::VertexShaderInfo& regs, const HW::Context& context,
                            std::array<ShaderVertexInputInfo, 3>& input_info);
ShaderParams PrepareProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               input_info);
ShaderParams PrepareProgram(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
                            ShaderComputeInputInfo& input_info);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_ */
