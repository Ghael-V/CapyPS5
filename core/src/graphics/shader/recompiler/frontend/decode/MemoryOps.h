// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

void DecodeSmem(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeMubuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction& inst);
void DecodeMtbuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction& inst);
void DecodeFlat(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst);
void DecodeDs(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MEMORYOPS_H_ */
