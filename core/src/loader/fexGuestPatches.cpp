// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "loader/fexGuestPatches.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <vector>

namespace Loader {

uint64_t PatchFexUnsupportedInstructions(uint64_t segment_addr, uint64_t segment_size,
                                         std::span<const uintptr_t> function_starts) {
	if (segment_size == 0 || function_starts.empty()) {
		return 0;
	}
	const uint64_t segment_end = segment_addr + segment_size;
	std::vector<uintptr_t> starts;
	for (const auto start: function_starts) {
		if (start >= segment_addr && start < segment_end) {
			starts.push_back(start);
		}
	}
	std::sort(starts.begin(), starts.end());
	starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

	ZydisDecoder decoder;
	if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
		return 0;
	}
	uint64_t patched = 0;
	for (size_t i = 0; i < starts.size(); i++) {
		const uint64_t end = i + 1 < starts.size() ? starts[i + 1] : segment_end;
		for (uint64_t address = starts[i]; address < end;) {
			ZydisDecodedInstruction instruction;
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr,
			                                                reinterpret_cast<const void*>(address),
			                                                end - address, &instruction))) {
				// Undecodable bytes (padding or data at the end of a function): stop here.
				break;
			}
			if ((instruction.mnemonic == ZYDIS_MNEMONIC_MONITORX ||
			     instruction.mnemonic == ZYDIS_MNEMONIC_MWAITX) &&
			    instruction.length == 3) {
				auto* bytes = reinterpret_cast<uint8_t*>(address);
				bytes[0]    = 0x0f; // nop dword ptr [rax]
				bytes[1]    = 0x1f;
				bytes[2]    = 0x00;
				patched++;
			}
			address += instruction.length;
		}
	}
	return patched;
}

} // namespace Loader
