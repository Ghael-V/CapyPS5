// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_TELEMETRY_H
#define COMMON_TELEMETRY_H

#include <atomic>
#include <chrono>
#include <cstdint>

// Lock-free counters sampled by the frontend performance overlay. Producers only increment;
// the overlay computes rates from deltas between samples.
namespace Common::Telemetry {

// Guest flips that reached the screen (the game's own frame rate).
inline std::atomic<uint64_t> g_game_frames {0};
inline std::atomic<uint64_t> g_last_game_frame_ns {0};
// Worst frame time since the overlay last exchanged it with 0.
inline std::atomic<uint64_t> g_max_frame_ns {0};
// Host swapchain presents (can differ from game frames when the presenter repeats images).
inline std::atomic<uint64_t> g_host_presents {0};
// Places where emulation papered over a problem; non-zero rates point at real bugs.
inline std::atomic<uint64_t> g_wait_reg_mem_timeouts {0};
inline std::atomic<uint64_t> g_wait_reg_mem_unmapped {0};
inline std::atomic<uint64_t> g_null_buffer_fallbacks {0};

inline void RecordGameFrame() {
	const auto now  = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
	const auto prev = g_last_game_frame_ns.exchange(now, std::memory_order_relaxed);
	if (prev != 0 && now > prev) {
		const auto delta = now - prev;
		auto       max   = g_max_frame_ns.load(std::memory_order_relaxed);
		while (delta > max && !g_max_frame_ns.compare_exchange_weak(max, delta, std::memory_order_relaxed)) {
		}
	}
	g_game_frames.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Common::Telemetry

#endif /* COMMON_TELEMETRY_H */
