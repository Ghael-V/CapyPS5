// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	// `force` queries the driver now; otherwise at most every RefreshIntervalNs (see .cpp).
	void Refresh(bool force = false);
	void Wait(uint64_t tick);

	// Fence tracking (see the .cpp): a fence for the submission of `tick`, or null when the
	// timeline semaphore tracks progress instead. Call under the queue lock; after a successful
	// submit pass the fence to Track.
	[[nodiscard]] vk::Fence AcquireFence();
	void                    Track(uint64_t tick, vk::Fence fence);
	[[nodiscard]] bool      UsesFences() const noexcept { return m_use_fences; }

private:
	void WaiterRun();

	GraphicContext&       m_graphics;
	vk::Semaphore         m_semaphore = nullptr;
	bool                  m_use_fences = false;
	std::mutex            m_fence_mutex;
	std::condition_variable m_fence_cv;
	std::deque<std::pair<uint64_t, vk::Fence>> m_pending_fences;
	std::vector<vk::Fence> m_free_fences;
	bool                  m_stop = false;
	std::thread           m_waiter;
	std::atomic<int64_t>  m_last_refresh_ns {0};
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
