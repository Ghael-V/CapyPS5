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
// CapyPS5: optional second level for a 40-bit guest layout (PS4: user space up to 0xfbffffffff).
// When an address has this bit set, the bit below it is cleared too, so the top half of the
// guest space folds into the guest-owned window below the host's libraries instead of onto them.
// Zero disables it.
inline std::atomic<uint64_t> GuestAddressFoldHighBit {0};

inline uint64_t FoldGuestAddressValue(uint64_t Address, uint64_t Mask, uint64_t HighBit) {
  return (Address & Mask) & ~((Address & HighBit) >> 1);
}
} // namespace FEXCore
