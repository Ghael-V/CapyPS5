// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/debugFlags.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <chrono>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	// A fault we cannot clear retries immediately and forever. Legitimate repeats (the guest
	// rewriting a page the GPU re-protected) are spread across frames, so only a burst of
	// back-to-back faults on one page counts as a loop.
	static thread_local uint64_t last_fault_page = 0;
	static thread_local uint32_t repeat_count    = 0;
	static thread_local std::chrono::steady_clock::time_point last_fault_time {};
	const uint64_t fault_page = fault_vaddr >> 12u;
	const auto     now        = std::chrono::steady_clock::now();
	repeat_count = (fault_page == last_fault_page && now - last_fault_time < std::chrono::milliseconds(1))
	                   ? repeat_count + 1
	                   : 0;
	last_fault_page = fault_page;
	last_fault_time = now;
	if (repeat_count > 1000) {
		LOGF("RenderContext::HandleFault: unresolvable %s fault at 0x%016" PRIx64 "\n",
		     access == PageFaultAccess::Write ? "write" : "read", fault_vaddr);
		repeat_count = 0;
		return false;
	}
	if (access == PageFaultAccess::Write) {
		// Games fill dynamic buffers page after page, and each 4 KiB page faulted on its own
		// (signal, mprotect, tracker lock contended by Stray's task threads). Release a whole
		// window around the fault when nothing in it is GPU-written (that would need a download).
		// `setprop debug.capy.write_fault_kb N` sets the window (4 = one page as before).
		static Common::DebugFlags::CachedValue write_fault_kb("debug.capy.write_fault_kb", 64);
		const uint64_t window = static_cast<uint64_t>(std::max<int64_t>(write_fault_kb.Get(), 4)) * 1024u;
		const uint64_t window_lo = fault_vaddr & ~(window - 1u);
		if (window > 4096 && (window & (window - 1u)) == 0 && IsMapped(window_lo, window) &&
		    !m_buffer_cache.IsRegionGpuModified(window_lo, window)) {
			m_buffer_cache.InvalidateMemory(window_lo, window);
		} else {
			m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		}
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		// Upstream behaviour: ReadMemory also clears stale GPU-dirty protection when there is
		// nothing to download, so the faulting access can be retried.
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size, bool discard) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		if (!m_mapped_ranges.Intersects(vaddr, size)) {
			return;
		}
	}
	const auto unmap = [this, vaddr, size, discard] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		// Only GPU-written data must be read back (finishing the GPU) before the range goes away;
		// images are destroyed after the GPU is done with them. Finishing on every registered
		// range made Monster Boy, which unmaps memory the GPU used every frame, wait for the
		// whole GPU each frame (14 FPS). `debug.capy.unmap_finish 1` restores that.
		static Common::DebugFlags::CachedValue unmap_finish("debug.capy.unmap_finish", 0);
		const bool needs_gpu =
		    unmap_finish.Get() != 0
		        ? (m_buffer_cache.IsRegionRegistered(vaddr, size) || m_texture_cache.IsRegionRegistered(vaddr, size))
		        : !discard && m_buffer_cache.IsRegionGpuModified(vaddr, size);
		// Pending priority work is mostly flip and end-of-pipe notifications; when the contents are
		// discarded only an asynchronous download into the range still has to land first.
		const bool pending = discard ? m_buffer_cache.HasAsyncDownloadIn(vaddr, size)
		                             : m_command_scheduler.HasPendingPriorityOperations();
		if (m_command_scheduler.Active() && (needs_gpu || pending)) {
#if defined(__ANDROID__)
			{
				static Common::DebugFlags::CachedValue unmap_log("debug.capy.unmap_log", 0);
				if (unmap_log.Get() != 0) {
					__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Unmap", "finish for 0x%llx+0x%llx discard=%d gpu=%d priority=%d",
					                    static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size), discard ? 1 : 0,
					                    needs_gpu ? 1 : 0, m_command_scheduler.HasPendingPriorityOperations() ? 1 : 0);
				}
			}
#endif
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		if (discard) {
			m_buffer_cache.DiscardGpuWrites(vaddr, size);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	// Only memory the CPU wrote since the previous call can need an upload; walking every
	// buffer of every mapping per draw took a fifth of the GPU thread in UE4 games.
	// `setprop debug.capy.bda_full_sync 1` walks everything as before.
	static Common::DebugFlags::CachedValue full_sync("debug.capy.bda_full_sync", 0);
	// The guest CPU writes what a submission reads before submitting it, so one synchronization
	// per submission slice suffices; per draw it re-protected pages the game kept writing, and
	// every write faulted again. `setprop debug.capy.bda_sync_per_draw 1` syncs on every call.
	static Common::DebugFlags::CachedValue per_draw("debug.capy.bda_sync_per_draw", 0);
	if (!m_bda_sync_requested && per_draw.Get() == 0 && full_sync.Get() == 0) {
		m_fault_process_pending = true;
		return;
	}
	m_bda_sync_requested = false;
	auto modified = m_buffer_cache.TakeCpuModifiedRanges();
	if (full_sync.Get() != 0 || !m_bda_synced_once) {
		m_bda_synced_once = true;
		std::shared_lock lock(m_mapped_ranges_mutex);
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
		});
	} else {
		modified.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
		});
	}
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
