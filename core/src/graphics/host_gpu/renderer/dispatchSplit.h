// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef GRAPHICS_HOST_GPU_RENDERER_DISPATCH_SPLIT_H
#define GRAPHICS_HOST_GPU_RENDERER_DISPATCH_SPLIT_H

// Guest dispatches may ask for more workgroups along an axis than the host allows
// (maxComputeWorkGroupCount; 65535 on Adreno, Astro Bot dispatches 131072 x 1 x 1). Recording
// them as one vkCmdDispatch is undefined behaviour, and vkCmdDispatchBase cannot reach past the
// limit either (baseGroup + groupCount must stay within it). They are recorded as plain
// vkCmdDispatch pieces within the limits; the translated shader adds the piece's base (push data,
// Bindings::group_base_dword) to gl_WorkGroupID and gl_GlobalInvocationID.
// From tarek369/KytyPS5-Android 0835ee42, ported by hand.

#include <algorithm>
#include <array>
#include <cstdint>

namespace Libs::Graphics {

using DispatchGroups = std::array<uint32_t, 3>;

struct DispatchPiece {
	DispatchGroups base {};
	DispatchGroups count {};
};

[[nodiscard]] constexpr bool DispatchFits(const DispatchGroups& groups, const DispatchGroups& limits) {
	return groups[0] <= limits[0] && groups[1] <= limits[1] && groups[2] <= limits[2];
}

// Calls emit(const DispatchPiece&) for every piece (x fastest) and returns the piece count.
// An empty dispatch (any count 0) has no pieces.
template <typename Emit>
uint32_t ForEachDispatchPiece(const DispatchGroups& groups, const DispatchGroups& limits, Emit&& emit) {
	DispatchGroups pieces {};
	DispatchGroups steps {};
	for (uint32_t axis = 0; axis < 3; axis++) {
		steps[axis]  = std::max(limits[axis], 1u);
		pieces[axis] = groups[axis] == 0 ? 0u : (groups[axis] - 1u) / steps[axis] + 1u;
		if (pieces[axis] == 0) {
			return 0;
		}
	}
	uint32_t emitted = 0;
	for (uint32_t z = 0; z < pieces[2]; z++) {
		for (uint32_t y = 0; y < pieces[1]; y++) {
			for (uint32_t x = 0; x < pieces[0]; x++) {
				const DispatchGroups index {x, y, z};
				DispatchPiece        piece {};
				for (uint32_t axis = 0; axis < 3; axis++) {
					piece.base[axis]  = index[axis] * steps[axis];
					piece.count[axis] = std::min(steps[axis], groups[axis] - piece.base[axis]);
				}
				emit(piece);
				emitted++;
			}
		}
	}
	return emitted;
}

} // namespace Libs::Graphics

#endif // GRAPHICS_HOST_GPU_RENDERER_DISPATCH_SPLIT_H
