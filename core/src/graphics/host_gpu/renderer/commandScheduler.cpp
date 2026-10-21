// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/commandScheduler.h"
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif
#include "graphics/guest_gpu/graphicsRun.h"

#include "graphics/host_gpu/renderer/gpuProfiler.h"
#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__) || defined(ANDROID)
#include <android/log.h>
#include <dlfcn.h>
#include <unwind.h>
#define CS_LOG(...)
#define CS_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Scheduler", __VA_ARGS__)
#else
#define CS_LOG(...)
#define CS_ERR(...)
#endif

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

#if defined(__ANDROID__)
// `setprop debug.capy.submit_stats 1`: every 2 s log which call chains submit, and how long the
// CPU waited on the GPU from each (tag CapyPS5-Submit), to find serializing waits.
namespace SubmitStats {

struct Entry {
	uint32_t submits = 0;
	uint32_t waits   = 0;
	uint64_t wait_ns = 0;
};

struct Trace {
	std::array<uintptr_t, 9> frames {};
	size_t                   count = 0;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* arg) {
	auto* trace = static_cast<Trace*>(arg);
	if (trace->count >= trace->frames.size()) {
		return _URC_END_OF_STACK;
	}
	trace->frames[trace->count++] = _Unwind_GetIP(context);
	return _URC_NO_REASON;
}

bool Enabled() {
	static Common::DebugFlags::CachedValue enabled("debug.capy.submit_stats", 0);
	return enabled.Get() != 0;
}

std::mutex                                         g_mutex;
std::unordered_map<std::string, Entry>             g_entries;
std::chrono::steady_clock::time_point              g_report = std::chrono::steady_clock::now();

std::string Key() {
	Trace trace;
	_Unwind_Backtrace(CollectFrame, &trace);
	std::string key;
	// Skip this function and the scheduler entry point itself.
	for (size_t i = 2; i < trace.count; i++) {
		Dl_info info {};
		const auto base = dladdr(reinterpret_cast<void*>(trace.frames[i]), &info) != 0
		                      ? reinterpret_cast<uintptr_t>(info.dli_fbase)
		                      : 0;
		char frame[32];
		std::snprintf(frame, sizeof(frame), " +0x%llx", static_cast<unsigned long long>(trace.frames[i] - base));
		key += frame;
	}
	return key;
}

void Report() {
	const auto now = std::chrono::steady_clock::now();
	if (now < g_report + std::chrono::seconds(2)) {
		return;
	}
	g_report = now;
	std::vector<std::pair<std::string, Entry>> sorted(g_entries.begin(), g_entries.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
		return a.second.wait_ns != b.second.wait_ns ? a.second.wait_ns > b.second.wait_ns
		                                            : a.second.submits > b.second.submits;
	});
	uint64_t total_ns = 0;
	uint32_t total_submits = 0;
	for (const auto& [key, entry]: sorted) {
		total_ns += entry.wait_ns;
		total_submits += entry.submits;
	}
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Submit", "---- %u submits, CPU waited %.1f ms on the GPU",
	                    total_submits, static_cast<double>(total_ns) / 1e6);
	for (size_t i = 0; i < sorted.size() && i < 10; i++) {
		const auto& [key, entry] = sorted[i];
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Submit", "%7.1f ms waits %-4u submits %-4u from%s",
		                    static_cast<double>(entry.wait_ns) / 1e6, entry.waits, entry.submits, key.c_str());
	}
	g_entries.clear();
}

void NoteSubmit() {
	if (!Enabled()) {
		return;
	}
	auto key = Key();
	std::lock_guard lock(g_mutex);
	g_entries[key].submits++;
	Report();
}

void NoteWait(std::chrono::steady_clock::time_point start) {
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	auto key = Key();
	std::lock_guard lock(g_mutex);
	auto& entry = g_entries[key];
	entry.waits++;
	entry.wait_ns += static_cast<uint64_t>(ns);
}

} // namespace SubmitStats
#define SUBMIT_STATS_WAIT(expr)                                                                     \
	do {                                                                                           \
		if (SubmitStats::Enabled()) {                                                              \
			const auto submit_stats_start = std::chrono::steady_clock::now();                      \
			expr;                                                                                  \
			SubmitStats::NoteWait(submit_stats_start);                                             \
		} else {                                                                                   \
			expr;                                                                                  \
		}                                                                                          \
	} while (false)
#else
#define SUBMIT_STATS_WAIT(expr) expr
#endif

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	CS_LOG("Begin called: registers=%p by %p", &registers, __builtin_return_address(0));
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (GpuProfiler::Enabled() && Active() && !m_command.IsInvalid() && Current().IsRendering()) {
		GpuProfiler::SetEndCaller(__builtin_return_address(0));
	}
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	SUBMIT_STATS_WAIT(m_master.Wait(tick));
	BeginNext();
}

void CommandScheduler::Finish() {
	CS_LOG("Finish called by %p", __builtin_return_address(0));
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	SUBMIT_STATS_WAIT(m_master.Wait(CurrentTick() - 1));
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		SUBMIT_STATS_WAIT(m_master.Wait(tick));
		BeginNext();
	} else {
		SUBMIT_STATS_WAIT(m_master.Wait(tick));
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CS_LOG("QueueOperation called by %p priority=%d", __builtin_return_address(0), (int)priority);
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		GpuSyncRecord("QueueOperation waits for close");
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
#if defined(__ANDROID__) || defined(__linux__)
	pthread_setname_np(pthread_self(), "CapyGpuDeferred");
#endif
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	GpuSyncRecord("DrainPriorityOperations begin");
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
	GpuSyncRecord("DrainPriorityOperations end");
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	GpuSyncRecord("WaitPriorityOperations begin", tick);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
	GpuSyncRecord("WaitPriorityOperations end", tick);
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	if (!Active()) {
		void* caller = __builtin_return_address(0);
		Dl_info info {};
		dladdr(caller, &info);
		CS_ERR("CheckActive FAILED: caller=%p (%s in %s)", caller,
		       info.dli_sname ? info.dli_sname : "unknown",
		       info.dli_fname ? info.dli_fname : "unknown");
	}
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	EXIT_IF(m_command.IsInvalid());
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	m_dynamic_state.Invalidate();
	return m_command;
}

std::atomic<uint64_t> g_submit_count {0};

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	g_submit_count.fetch_add(1, std::memory_order_relaxed);
#if defined(__ANDROID__)
	SubmitStats::NoteSubmit();
#endif
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	if (GpuProfiler::Enabled()) {
		GpuProfiler::Collect(m_graphics);
	}
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
