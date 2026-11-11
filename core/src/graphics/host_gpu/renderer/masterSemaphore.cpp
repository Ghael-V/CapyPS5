// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "common/debugFlags.h"

#include <chrono>
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

	// Turnip on KGSL emulates timeline semaphores: every submission that signals one cleans up
	// the timeline with zero-timeout kernel waits, which sleep while the GPU is busy (~3 ms per
	// submission, about half the GPU thread in God of War at ~160 submits/s). Progress is
	// tracked with one fence per submission instead, waited in order by a thread of its own, so
	// polling it is an atomic read. `debug.capy.fence_tracking 0` restores the timeline.
	static Common::DebugFlags::CachedValue fence_tracking("debug.capy.fence_tracking", 1);
	m_use_fences = fence_tracking.Get() != 0;
	if (m_use_fences) {
		m_waiter = std::thread([this] { WaiterRun(); });
	}
}

vk::Fence MasterSemaphore::AcquireFence() {
	if (!m_use_fences) {
		return nullptr;
	}
	{
		std::lock_guard lock(m_fence_mutex);
		if (!m_free_fences.empty()) {
			const auto fence = m_free_fences.back();
			m_free_fences.pop_back();
			return fence;
		}
	}
	vk::FenceCreateInfo create_info {};
	vk::Fence           fence  = nullptr;
	const auto          result = m_graphics.device.createFence(&create_info, nullptr, &fence);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || fence == nullptr);
	return fence;
}

void MasterSemaphore::Track(uint64_t tick, vk::Fence fence) {
	if (fence == nullptr) {
		return;
	}
	{
		std::lock_guard lock(m_fence_mutex);
		m_pending_fences.emplace_back(tick, fence);
	}
	m_fence_cv.notify_all();
}

void MasterSemaphore::WaiterRun() {
	std::unique_lock lock(m_fence_mutex);
	for (;;) {
		m_fence_cv.wait(lock, [this] { return m_stop || !m_pending_fences.empty(); });
		if (m_pending_fences.empty()) {
			return;
		}
		const auto [tick, fence] = m_pending_fences.front();
		lock.unlock();
		const auto result = m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX);
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
		(void)m_graphics.device.resetFences(1, &fence);
		lock.lock();
		m_pending_fences.pop_front();
		m_free_fences.push_back(fence);
		auto known = m_gpu_tick.load(std::memory_order_acquire);
		while (known < tick && !m_gpu_tick.compare_exchange_weak(known, tick, std::memory_order_release,
		                                                         std::memory_order_relaxed)) {
		}
		m_fence_cv.notify_all();
	}
}

MasterSemaphore::~MasterSemaphore() {
	if (m_waiter.joinable()) {
		{
			std::lock_guard lock(m_fence_mutex);
			m_stop = true;
		}
		m_fence_cv.notify_all();
		m_waiter.join();
		for (const auto& [tick, fence]: m_pending_fences) {
			m_graphics.device.destroyFence(fence, nullptr);
		}
		for (const auto fence: m_free_fences) {
			m_graphics.device.destroyFence(fence, nullptr);
		}
	}
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh(bool force) {
	if (m_use_fences) {
		return; // the waiter thread keeps m_gpu_tick current
	}
	// Turnip on KGSL emulates timeline semaphores: reading the value garbage-collects the
	// timeline and can block in the kernel. Opportunistic polls (IsFree, deferred operations: one
	// per draw or dispatch) took ~65% of the GPU thread in God of War; a value up to 2 ms stale
	// only delays reuse. Waits still query it exactly.
	constexpr int64_t RefreshIntervalNs = 2'000'000;
	const int64_t     now = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (!force && now - m_last_refresh_ns.load(std::memory_order_relaxed) < RefreshIntervalNs) {
		return;
	}
	m_last_refresh_ns.store(now, std::memory_order_relaxed);
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
	if (m_use_fences) {
		std::unique_lock lock(m_fence_mutex);
		m_fence_cv.wait(lock, [&] { return IsFree(tick); });
		return;
	}
	Refresh(true);
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
	Refresh(true);
}

} // namespace Libs::Graphics
