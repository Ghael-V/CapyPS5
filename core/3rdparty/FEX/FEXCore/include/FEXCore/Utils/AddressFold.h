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
} // namespace FEXCore
