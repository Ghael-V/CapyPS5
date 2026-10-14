#ifndef COMMON_DEBUG_FLAGS_H
#define COMMON_DEBUG_FLAGS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#if defined(__ANDROID__)
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
	FlagCount
};

inline constexpr const char* kFlagNames[FlagCount] = {
    "debug.capy.force_sync", "debug.capy.no_stream", "debug.capy.idle_present",
    "debug.capy.idle_flush",
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

/** Numeric debug property, e.g. `static CachedValue wait("debug.capy.wait_us", 2000);`. */
class CachedValue {
public:
	constexpr CachedValue(const char* name, int64_t fallback): m_name(name), m_value(fallback), m_fallback(fallback) {}

	int64_t Get() {
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		                     std::chrono::steady_clock::now().time_since_epoch())
		                     .count();
		auto next = m_next_refresh_ms.load(std::memory_order_relaxed);
		if (now >= next && m_next_refresh_ms.compare_exchange_strong(next, now + 250)) {
			int64_t value = m_fallback;
#if defined(__ANDROID__)
			char text[PROP_VALUE_MAX] = {};
			if (__system_property_get(m_name, text) > 0) {
				char* end    = nullptr;
				const auto v = std::strtoll(text, &end, 10);
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
	int64_t              m_fallback;
};

/** True when `adb shell setprop debug.capy.verbose 1` asks for detailed (hot path) logging. */
inline bool Verbose() {
	static CachedValue verbose("debug.capy.verbose", 0);
	return verbose.Get() != 0;
}

} // namespace Common::DebugFlags

#endif /* COMMON_DEBUG_FLAGS_H */
