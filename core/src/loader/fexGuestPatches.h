// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_LOADER_FEX_GUEST_PATCHES_H_
#define KYTY_LOADER_FEX_GUEST_PATCHES_H_

#include <cstdint>
#include <span>

namespace Loader {

// Guest instructions the PS5 CPU (Zen 2) runs that FEXCore treats as privileged, rewritten in
// place before the JIT sees them. Today: AMD MONITORX and MWAITX (spin-wait hints, used by Wwise
// and others), which become 3-byte NOPs; MWAITX may wake at any time, so the waiting loop just
// spins. Each function is decoded from its eh_frame start, so data bytes are never touched.
// Returns the number of instructions rewritten. The segment must be writable.
uint64_t PatchFexUnsupportedInstructions(uint64_t segment_addr, uint64_t segment_size,
                                         std::span<const uintptr_t> function_starts);

} // namespace Loader

#endif /* KYTY_LOADER_FEX_GUEST_PATCHES_H_ */
