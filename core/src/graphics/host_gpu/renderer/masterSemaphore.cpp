// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cinttypes>

#if defined(__ANDROID__) || defined(ANDROID)
#include <android/log.h>
#endif

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	GpuSyncRecord("MasterSemaphore wait", tick, m_current_tick.load());
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// Returning before the GPU reached `tick` would let callers reuse resources still in use,
	// so only report a long stall and keep waiting.
	constexpr uint64_t warn_ns = 3'000'000'000ull;
	auto result = m_graphics.device.waitSemaphores(&wait_info, warn_ns);
	if (result == vk::Result::eTimeout) {
#if defined(__ANDROID__) || defined(ANDROID)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Gpu",
		                    "MasterSemaphore::Wait: tick %" PRIu64 " not reached after 3 s"
		                    " (current=%" PRIu64 ", gpu=%" PRIu64 "), still waiting",
		                    tick, CurrentTick(), KnownGpuTick());
#endif
		result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
