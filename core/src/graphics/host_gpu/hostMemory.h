// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORY_H_

#include "common/common.h"

namespace Libs::Graphics {

enum class HostMemoryAccess { Read, Mapped };

bool HostMemoryQueryRange(uint64_t addr, uint64_t requested_size, HostMemoryAccess access,
                          uint64_t& accessible_size);
bool HostMemoryQueryReadable(uint64_t addr, uint64_t requested_size, uint64_t& readable_size);
bool HostMemoryIsReadable(uint64_t addr);
bool HostMemoryRangeIsReadable(uint64_t addr, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORY_H_
