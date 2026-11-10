// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

namespace FEXCore {
// CapyPS5: mask applied to every guest data address by code compiled while it is non-zero.
// Games built for a 48-bit address space (Unreal Engine 4 on PS5 reserves 512 GiB and uses its
// last GiB) touch addresses a 39-bit host cannot map; masking folds them onto a host window.
// Host addresses are below the mask, so they are unaffected.
inline std::atomic<uint64_t> GuestAddressFoldMask {0};
// CapyPS5: guest window remap table for a 40-bit guest layout (PS4: user space up to
// 0xfbffffffff). When non-zero, it is the host address of 2^19 int64 deltas, one per 2 MiB guest
// chunk: code compiled meanwhile accesses `address + table[(address >> 21) & 0x7ffff]` instead of
// masking. The emulator points chunks the host cannot back at blocks of its own space. The
// address must be an AArch64 logical immediate with bits 0-21 clear (the backend ORs the index in).
inline std::atomic<uint64_t> GuestAddressRemapTable {0};
inline constexpr uint32_t GuestAddressRemapShift = 21;
inline constexpr uint32_t GuestAddressRemapBits = 19;
} // namespace FEXCore
