// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/gpuProfiler.h"

#include "common/debugFlags.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Libs::Graphics::GpuProfiler {

namespace {

constexpr uint32_t QueryCount = 8192;

struct Scope {
	uint32_t    query = 0;
	uint32_t    draws = 0;
	std::string label;
};

struct Stat {
	uint64_t ns        = 0;
	uint32_t count     = 0;
	uint32_t draws     = 0;
	uint64_t fragments = 0;
	uint64_t compute   = 0;
};

struct State {
	vk::QueryPool                         pool = nullptr;
	// Pipeline statistics, one query per scope (timestamp query / 2), when the device has them.
	vk::QueryPool                         stats_pool = nullptr;
	uint32_t                              next = 0;
	std::deque<Scope>                     pending;
	bool                                  open = false;
	char                                  pass_label[96] {};
	std::unordered_map<std::string, Stat> stats;
	uint64_t                              total_ns = 0;
	std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();
	uint64_t                              end_caller = 0;
	GraphicContext*                       graphics   = nullptr;
	std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now();
	bool                                  watchdog = false;
};

// The GPU thread and the present thread both record and submit.
std::mutex g_mutex;

State& GetState() {
	static State state;
	return state;
}

bool Begin(GraphicContext& graphics, vk::CommandBuffer command, std::string label) {
	auto& state = GetState();
	state.graphics = &graphics;
	if (!state.watchdog) {
		// The GPU thread blocks inside vkQueueSubmit once a hung GPU fills the ring, so a separate
		// thread reports which scope never finished.
		state.watchdog = true;
		std::thread([] {
			bool reported = false;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				bool stalled = false;
				{
					std::lock_guard lock(g_mutex);
					auto& s = GetState();
					stalled = !s.pending.empty() &&
					          std::chrono::steady_clock::now() - s.progress > std::chrono::seconds(4);
				}
				if (stalled && !reported) {
					ReportHang();
				}
				reported = stalled;
			}
		}).detach();
	}
	if (state.open) {
		return false;
	}
	if (state.pool == nullptr) {
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = QueryCount;
		if (graphics.device.createQueryPool(&info, nullptr, &state.pool) != vk::Result::eSuccess) {
			state.pool = nullptr;
			return false;
		}
	}
	if (state.pending.size() >= QueryCount / 2 - 1) {
		// Results stopped arriving; drop the oldest rather than reuse queries still in flight.
		state.pending.pop_front();
	}
	if (state.next + 2 > QueryCount) {
		state.next = 0;
	}
	const auto query = state.next;
	state.next += 2;
	command.resetQueryPool(state.pool, query, 2);
	if (state.stats_pool == nullptr && graphics.pipeline_statistics_enabled) {
		vk::QueryPoolCreateInfo info {};
		info.queryType          = vk::QueryType::ePipelineStatistics;
		info.queryCount         = QueryCount / 2;
		info.pipelineStatistics = vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
		                          vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
		if (graphics.device.createQueryPool(&info, nullptr, &state.stats_pool) != vk::Result::eSuccess) {
			state.stats_pool = nullptr;
		}
	}
	if (state.stats_pool != nullptr) {
		command.resetQueryPool(state.stats_pool, query / 2, 1);
	}
	// `debug.capy.gpu_profile 3`: drain the queue before each scope (passes begin before
	// vkCmdBeginRendering). Adreno does not hold bottom-of-pipe timestamps behind earlier work,
	// so without this a scope absorbs the tail of whatever ran before it.
	// Mode 4 (fragment counts) drains too: Adreno pipeline statistics also count overlapped work.
	static Common::DebugFlags::CachedValue mode("debug.capy.gpu_profile", 0);
	if (mode.Get() == 3 || mode.Get() == 4) {
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                        vk::PipelineStageFlagBits::eAllCommands, {}, barrier, {}, {});
	}
	// Bottom of pipe: written once earlier work finishes, so a scope does not absorb its tail.
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool, query);
	if (state.stats_pool != nullptr) {
		command.beginQuery(state.stats_pool, query / 2, {});
	}
	state.pending.push_back({query, 0, std::move(label)});
	state.open = true;
	return true;
}

} // namespace

bool Enabled() {
	static Common::DebugFlags::CachedValue enabled("debug.capy.gpu_profile", 0);
	return enabled.Get() != 0;
}

void SetPassLabel(uint64_t target, uint32_t width, uint32_t height, uint64_t pixel_shader) {
	std::lock_guard lock(g_mutex);
	std::snprintf(GetState().pass_label, sizeof(GetState().pass_label),
	              "pass %ux%u rt=0x%llx ps=0x%llx", width, height,
	              static_cast<unsigned long long>(target),
	              static_cast<unsigned long long>(pixel_shader));
}

void NoteDraw() {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	if (state.open && !state.pending.empty()) {
		state.pending.back().draws++;
	}
}

void BeginPass(GraphicContext& graphics, vk::CommandBuffer command, uint32_t width,
               uint32_t height, uint32_t colors, bool depth) {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	std::string label = state.pass_label[0] != 0 ? state.pass_label : "pass (unlabeled)";
	char        shape[48];
	std::snprintf(shape, sizeof(shape), " %ux%u c%u%s", width, height, colors, depth ? "+d" : "");
	label += shape;
	state.pass_label[0] = 0;
	(void)Begin(graphics, command, std::move(label));
}

void BeginDispatch(GraphicContext& graphics, vk::CommandBuffer command, uint64_t compute_shader,
                   uint32_t groups) {
	std::lock_guard lock(g_mutex);
	char label[64];
	std::snprintf(label, sizeof(label), "dispatch cs=0x%llx groups=%u",
	              static_cast<unsigned long long>(compute_shader), groups);
	(void)Begin(graphics, command, label);
}

void SetEndCaller(const void* caller) {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	if (state.end_caller == 0) {
		Dl_info info {};
		const auto base = dladdr(caller, &info) != 0 ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
		state.end_caller = reinterpret_cast<uintptr_t>(caller) - base;
	}
}

void EndScope(vk::CommandBuffer command) {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	if (!state.open || state.pending.empty()) {
		return;
	}
	if (state.stats_pool != nullptr) {
		command.endQuery(state.stats_pool, state.pending.back().query / 2);
	}
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool,
	                       state.pending.back().query + 1);
	state.open = false;
#if defined(__ANDROID__)
	static Common::DebugFlags::CachedValue mode("debug.capy.gpu_profile", 0);
	if (mode.Get() == 2) {
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuPass", "draws %u %s ended by +0x%llx",
		                    state.pending.back().draws, state.pending.back().label.c_str(),
		                    static_cast<unsigned long long>(state.end_caller));
	}
#endif
	state.end_caller = 0;
}

void Collect(GraphicContext& graphics) {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	if (state.pool == nullptr) {
		return;
	}
	const double period = graphics.GetPhysicalDeviceProperties().limits.timestampPeriod;
	// The open scope is still being recorded; only closed ones can have results.
	const size_t closed = state.pending.size() - (state.open ? 1u : 0u);
	size_t       done   = 0;
	for (; done < closed; done++) {
		auto& scope = state.pending[done];
		std::array<uint64_t, 4> values {};
		const auto result = graphics.device.getQueryPoolResults(
		    state.pool, scope.query, 2, sizeof(values), values.data(), 2 * sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		if ((result != vk::Result::eSuccess && result != vk::Result::eNotReady) || values[1] == 0 ||
		    values[3] == 0) {
			break;
		}
		const auto ns = values[2] > values[0]
		                    ? static_cast<uint64_t>(static_cast<double>(values[2] - values[0]) * period)
		                    : 0;
		std::array<uint64_t, 3> invocations {};
		if (state.stats_pool != nullptr) {
			// Fragment and compute shader invocations, then availability.
			(void)graphics.device.getQueryPoolResults(
			    state.stats_pool, scope.query / 2, 1, sizeof(invocations), invocations.data(),
			    sizeof(invocations), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		}
		auto& stat = state.stats[scope.label];
		stat.fragments += invocations[2] != 0 ? invocations[0] : 0;
		stat.compute += invocations[2] != 0 ? invocations[1] : 0;
		stat.ns += ns;
		stat.count++;
		stat.draws += scope.draws;
		state.total_ns += ns;
	}
	state.pending.erase(state.pending.begin(), state.pending.begin() + static_cast<std::ptrdiff_t>(done));
	if (done != 0) {
		state.progress = std::chrono::steady_clock::now();
	}

	const auto now = std::chrono::steady_clock::now();
	if (now < state.report + std::chrono::seconds(2)) {
		return;
	}
	const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - state.report).count();
	state.report       = now;
	std::vector<std::pair<std::string, Stat>> sorted(state.stats.begin(), state.stats.end());
	// `gpu_profile 4` ranks by shaded fragments (pipeline statistics) instead of time.
	static Common::DebugFlags::CachedValue mode("debug.capy.gpu_profile", 0);
	const bool by_fragments  = mode.Get() == 4;
	uint64_t   all_fragments = 0;
	for (const auto& [label, stat]: sorted) {
		all_fragments += stat.fragments;
	}
	std::sort(sorted.begin(), sorted.end(), [by_fragments](const auto& a, const auto& b) {
		return by_fragments ? a.second.fragments > b.second.fragments : a.second.ns > b.second.ns;
	});
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuProf",
	                    "---- measured GPU %.1f ms of %.1f ms wall (%zu kinds), %.1f M fragments",
	                    static_cast<double>(state.total_ns) / 1e6, static_cast<double>(wall_ns) / 1e6,
	                    sorted.size(), static_cast<double>(all_fragments) / 1e6);
	for (size_t i = 0; i < sorted.size() && i < 16; i++) {
		const auto& [label, stat] = sorted[i];
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuProf",
		                    "%7.1f ms x%-4u draws %-5u frags %7.1fM cs %7.1fM %s",
		                    static_cast<double>(stat.ns) / 1e6, stat.count, stat.draws,
		                    static_cast<double>(stat.fragments) / 1e6,
		                    static_cast<double>(stat.compute) / 1e6, label.c_str());
	}
#endif
	state.stats.clear();
	state.total_ns = 0;
}

void ReportHang() {
	std::lock_guard lock(g_mutex);
	auto& state = GetState();
	if (state.pool == nullptr || state.graphics == nullptr) {
		return;
	}
	// Scopes are collected in order, so the hung one is among the first still pending: its begin
	// timestamp is written and its end is not.
	for (size_t i = 0; i < state.pending.size() && i < 12; i++) {
		const auto& scope = state.pending[i];
		std::array<uint64_t, 4> values {};
		(void)state.graphics->device.getQueryPoolResults(
		    state.pool, scope.query, 2, sizeof(values), values.data(), 2 * sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuHang", "pending[%zu] begin=%d end=%d draws %u %s", i,
		                    values[1] != 0 ? 1 : 0, values[3] != 0 ? 1 : 0, scope.draws, scope.label.c_str());
#endif
	}
}

} // namespace Libs::Graphics::GpuProfiler
