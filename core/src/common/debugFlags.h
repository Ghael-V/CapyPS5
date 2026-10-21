// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_DEBUG_FLAGS_H
#define COMMON_DEBUG_FLAGS_H

#include <cstring>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

// Runtime debug switches for bisecting rendering bugs on device without rebuilding:
//   adb shell setprop debug.capy.<name> 1     (0 or empty disables)
// Values are re-read at most every 250 ms, so they can be toggled while a game runs.
namespace Common::DebugFlags {

enum Flag : uint32_t {
	ForceBufferSync = 0, // re-upload every CPU-readable buffer on each use
	NoStreamBuffer,      // never use the stream-buffer fast path for small buffers
	IdleBeforePresent,   // vkDeviceWaitIdle before presenting a frame
	IdleAfterFlush,      // vkDeviceWaitIdle after every guest command-buffer flush
	ForceLinearFilter,   // create every sampler with bilinear min/mag filtering
	FlagCount
};

inline constexpr const char* kFlagNames[FlagCount] = {
    "debug.capy.force_sync", "debug.capy.no_stream", "debug.capy.idle_present",
    "debug.capy.idle_flush", "debug.capy.force_linear",
};

inline std::atomic<uint32_t> g_mask {0};
inline std::atomic<int64_t>  g_next_refresh_ms {0};

inline uint32_t ReadMask() {
	uint32_t mask = 0;
#if defined(__ANDROID__)
	for (uint32_t i = 0; i < FlagCount; i++) {
		char value[PROP_VALUE_MAX] = {};
		if (__system_property_get(kFlagNames[i], value) > 0 && value[0] != '0') {
			mask |= 1u << i;
		}
	}
#endif
	return mask;
}

inline bool Enabled(Flag flag) {
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	auto next = g_next_refresh_ms.load(std::memory_order_relaxed);
	if (now >= next && g_next_refresh_ms.compare_exchange_strong(next, now + 250)) {
		g_mask.store(ReadMask(), std::memory_order_relaxed);
	}
	return (g_mask.load(std::memory_order_relaxed) & (1u << flag)) != 0;
}

// Options the frontend sets before a game starts (user and per-game settings). They take
// precedence over the system property of the same name; set only before the game runs.
struct FrontendOption {
	char    name[64] {};
	int64_t value = 0;
};
inline std::array<FrontendOption, 32> g_frontend_options {};
inline std::atomic<uint32_t>          g_frontend_option_count {0};

inline void SetFrontendOption(const char* name, int64_t value) {
	const auto count = g_frontend_option_count.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < count; i++) {
		if (std::strcmp(g_frontend_options[i].name, name) == 0) {
			g_frontend_options[i].value = value;
			return;
		}
	}
	if (count >= g_frontend_options.size() || std::strlen(name) >= sizeof(FrontendOption::name)) {
		return;
	}
	std::strcpy(g_frontend_options[count].name, name);
	g_frontend_options[count].value = value;
	g_frontend_option_count.store(count + 1, std::memory_order_release);
}

inline bool GetFrontendOption(const char* name, int64_t* value) {
	const auto count = g_frontend_option_count.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; i++) {
		if (std::strcmp(g_frontend_options[i].name, name) == 0) {
			*value = g_frontend_options[i].value;
			return true;
		}
	}
	return false;
}

/** Numeric debug property, e.g. `static CachedValue wait("debug.capy.wait_us", 2000);`. */
class CachedValue {
public:
	constexpr CachedValue(const char* name, int64_t fallback): m_name(name), m_value(fallback), m_fallback(fallback) {}

	int64_t Get() {
#if defined(__aarch64__)
		// Hot paths (every HLE call) read flags: compare against the virtual counter, which costs a
		// few cycles, so a value set while the game runs is seen within 250 ms on every thread (a
		// per-thread call count left threads that rarely call, like the flip thread at 2 FPS,
		// minutes behind).
		uint64_t ticks = 0;
		asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
		if (ticks < m_next_refresh_ticks.load(std::memory_order_relaxed)) {
			return m_value.load(std::memory_order_relaxed);
		}
		uint64_t frequency = 0;
		asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
		m_next_refresh_ticks.store(ticks + frequency / 4, std::memory_order_relaxed);
#else
		// Hot paths (every HLE call) read flags; only look at the clock every 256th call.
		static thread_local uint32_t calls = 0;
		if ((++calls & 255u) != 0 &&
		    m_next_refresh_ms.load(std::memory_order_relaxed) != 0) {
			return m_value.load(std::memory_order_relaxed);
		}
#endif
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		                     std::chrono::steady_clock::now().time_since_epoch())
		                     .count();
		auto next = m_next_refresh_ms.load(std::memory_order_relaxed);
		if (now >= next && m_next_refresh_ms.compare_exchange_strong(next, now + 250)) {
			int64_t value = m_fallback;
#if defined(__ANDROID__)
			char text[PROP_VALUE_MAX] = {};
			if (GetFrontendOption(m_name, &value)) {
				// Set by the frontend for this session.
			} else if (__system_property_get(m_name, text) > 0) {
				char* end    = nullptr;
				const auto v = std::strtoll(text, &end, 0);
				if (end != text) {
					value = v;
				}
			}
#endif
			m_value.store(value, std::memory_order_relaxed);
		}
		return m_value.load(std::memory_order_relaxed);
	}

private:
	const char*          m_name;
	std::atomic<int64_t> m_value;
	std::atomic<int64_t> m_next_refresh_ms {0};
	std::atomic<uint64_t> m_next_refresh_ticks {0};
	int64_t              m_fallback;
};

// Optional provenance probe for the 1080p RGBA image sampled by Unity's video overlay.
inline std::atomic<uint64_t> g_video_probe_address {0};
inline std::atomic<uint64_t> g_video_probe_size {0};
inline bool VideoProbeEnabled() {
	static CachedValue property("debug.capy.video_probe", 0);
	return property.Get() != 0;
}

// Guest video allocations watched by the optional AvPlayer texture diagnostic.
inline std::array<std::atomic<uint64_t>, 16> g_avplayer_video_addresses {};
inline std::array<std::atomic<uint64_t>, 16> g_avplayer_video_sizes {};
inline std::atomic<uint32_t> g_avplayer_video_count {0};

inline void RegisterAvPlayerVideoBuffer(uint64_t address, uint64_t size) {
	const auto index = g_avplayer_video_count.fetch_add(1, std::memory_order_relaxed);
	if (index < g_avplayer_video_addresses.size()) {
		g_avplayer_video_sizes[index].store(size, std::memory_order_relaxed);
		g_avplayer_video_addresses[index].store(address, std::memory_order_release);
	}
}

// Address being watched for writes (set automatically when a GPU wait on it times out).
inline std::atomic<uint64_t> g_watch_addr {0};

/** Logs writes that touch the watched address; used to find who should signal a label. */
inline void WatchWrite(const char* who, uint64_t addr, uint64_t size, uint64_t value) {
	const auto watch = g_watch_addr.load(std::memory_order_relaxed);
	if (watch == 0 || addr > watch + 7 || addr + size <= watch) {
		return;
	}
	static std::atomic<uint32_t> logged {0};
	if (logged.fetch_add(1) >= 64) {
		return;
	}
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Watch",
	                    "%s writes 0x%016llx size=%llu value=0x%llx (watching 0x%016llx)", who,
	                    static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
	                    static_cast<unsigned long long>(value), static_cast<unsigned long long>(watch));
#endif
}

/** Logs a warning the first time `key` is seen (at most 4096 distinct keys). */
inline void NoteOnce(const char* tag, uint64_t key, const char* fmt, ...) {
	static std::mutex            mutex;
	static std::vector<uint64_t> seen;
	{
		std::lock_guard lock(mutex);
		if (seen.size() >= 4096 || std::find(seen.begin(), seen.end(), key) != seen.end()) {
			return;
		}
		seen.push_back(key);
	}
#if defined(__ANDROID__)
	va_list args;
	va_start(args, fmt);
	__android_log_vprint(ANDROID_LOG_WARN, tag, fmt, args);
	va_end(args);
#else
	(void)tag;
	(void)fmt;
#endif
}

// One-frame trace: `setprop debug.capy.frame_trace 1` logs every draw/dispatch/texture of the
// next complete frame (flip to flip) under the CapyPS5-Frame tag. Reset the property to re-arm.
inline std::atomic<uint32_t> g_trace_state {0}; // 0 idle, 1 tracing, 2 done
inline std::atomic<uint32_t> g_trace_lines {0};
inline std::atomic<uint32_t> g_trace_draws {0}; // draws seen in the traced frame

inline bool FrameTracing() {
	return g_trace_state.load(std::memory_order_relaxed) == 1;
}

inline void FrameTrace(const char* fmt, ...) {
	// Skip early draws when diagnosing a late pass in a draw-heavy frame.
	static CachedValue trace_from("debug.capy.trace_from", 0);
	if (!FrameTracing() ||
	    g_trace_draws.load(std::memory_order_relaxed) < static_cast<uint32_t>(std::max<int64_t>(0, trace_from.Get())) ||
	    g_trace_lines.fetch_add(1, std::memory_order_relaxed) >= 20000) {
		return;
	}
#if defined(__ANDROID__)
	va_list args;
	va_start(args, fmt);
	__android_log_vprint(ANDROID_LOG_WARN, "CapyPS5-Frame", fmt, args);
	va_end(args);
#else
	(void)fmt;
#endif
}

inline void FrameTraceOnFlip() {
	static CachedValue property("debug.capy.frame_trace", 0);
	const auto         state = g_trace_state.load(std::memory_order_relaxed);
	if (property.Get() == 0) {
		g_trace_state.store(0, std::memory_order_relaxed);
		return;
	}
	if (state == 0) {
		g_trace_lines.store(0, std::memory_order_relaxed);
		g_trace_draws.store(0, std::memory_order_relaxed);
		g_trace_state.store(1, std::memory_order_relaxed);
		FrameTrace("---- frame begin ----");
	} else if (state == 1) {
		const auto draws = g_trace_draws.load(std::memory_order_relaxed);
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Frame", "frame end: %u draws", draws);
#endif
		g_trace_state.store(2, std::memory_order_relaxed);
	}
}

// `setprop debug.capy.trace_now N`: trace the next N draws right away (no flip needed; for games
// stuck before presenting). Change N to re-arm. Called once per draw.
inline void FrameTraceDrawTick() {
	static CachedValue trace_now("debug.capy.trace_now", 0);
	static int64_t     armed = 0;
	const auto         value = trace_now.Get();
	if (value != armed) {
		armed = value;
		if (value > 0) {
			g_trace_lines.store(0, std::memory_order_relaxed);
			g_trace_draws.store(0, std::memory_order_relaxed);
			g_trace_state.store(1, std::memory_order_relaxed);
			FrameTrace("---- trace_now begin ----");
		}
	}
	if (armed > 0 && g_trace_state.load(std::memory_order_relaxed) == 1 &&
	    static_cast<int64_t>(g_trace_draws.load(std::memory_order_relaxed)) >= armed) {
		FrameTrace("---- trace_now end ----");
		g_trace_state.store(2, std::memory_order_relaxed);
	}
}

/** True when `adb shell setprop debug.capy.verbose 1` asks for detailed (hot path) logging. */
inline bool Verbose() {
	static CachedValue verbose("debug.capy.verbose", 0);
	return verbose.Get() != 0;
}

/** `debug.capy.depthw_log 1`: logs every transfer write into a depth image (tag CapyPS5-DepthW). */
inline void LogDepthWrite(const char* who, uint64_t addr) {
	static CachedValue enabled("debug.capy.depthw_log", 0);
	if (enabled.Get() == 0) {
		return;
	}
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-DepthW", "%s addr=0x%llx", who,
	                    static_cast<unsigned long long>(addr));
#else
	(void)who;
	(void)addr;
#endif
}

} // namespace Common::DebugFlags

#endif /* COMMON_DEBUG_FLAGS_H */
