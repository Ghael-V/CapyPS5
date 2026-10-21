// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_PASS_INSPECTOR_H
#define COMMON_PASS_INSPECTOR_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// Debug aid behind the in-game "Capas" menu: records which pixel shaders drew during the last
// frame and lets the frontend switch individual ones off to find the pass behind an artifact.
namespace Common::PassInspector {

struct Pass {
	uint64_t pixel_shader = 0;
	uint32_t width        = 0; // first color target of the first draw using this shader
	uint32_t height       = 0;
	uint32_t targets      = 0;
	uint32_t draws        = 0;
};

inline std::mutex           g_mutex;
inline std::vector<Pass>    g_current;
inline std::vector<Pass>    g_last;
inline std::vector<uint64_t> g_skipped;
inline std::atomic<bool>    g_any_skipped {false};

inline void RecordDraw(uint64_t pixel_shader, uint32_t width, uint32_t height, uint32_t targets) {
	std::lock_guard lock(g_mutex);
	for (auto& pass: g_current) {
		if (pass.pixel_shader == pixel_shader) {
			pass.draws++;
			return;
		}
	}
	if (g_current.size() < 256) {
		g_current.push_back({pixel_shader, width, height, targets, 1});
	}
}

inline void OnFlip() {
	std::lock_guard lock(g_mutex);
	g_last.swap(g_current);
	g_current.clear();
}

inline bool IsSkipped(uint64_t pixel_shader) {
	if (!g_any_skipped.load(std::memory_order_relaxed)) {
		return false;
	}
	std::lock_guard lock(g_mutex);
	return std::find(g_skipped.begin(), g_skipped.end(), pixel_shader) != g_skipped.end();
}

inline void SetSkipped(uint64_t pixel_shader, bool skipped) {
	std::lock_guard lock(g_mutex);
	const auto it = std::find(g_skipped.begin(), g_skipped.end(), pixel_shader);
	if (skipped && it == g_skipped.end()) {
		g_skipped.push_back(pixel_shader);
	} else if (!skipped && it != g_skipped.end()) {
		g_skipped.erase(it);
	}
	g_any_skipped.store(!g_skipped.empty(), std::memory_order_relaxed);
}

inline void ClearSkipped() {
	std::lock_guard lock(g_mutex);
	g_skipped.clear();
	g_any_skipped.store(false, std::memory_order_relaxed);
}

/** Last frame's passes in draw order, plus skipped shaders that did not appear in it. */
inline std::vector<std::pair<Pass, bool>> Snapshot() {
	std::lock_guard                    lock(g_mutex);
	std::vector<std::pair<Pass, bool>> result;
	for (const auto& pass: g_last) {
		const bool skipped =
		    std::find(g_skipped.begin(), g_skipped.end(), pass.pixel_shader) != g_skipped.end();
		result.emplace_back(pass, skipped);
	}
	for (const auto shader: g_skipped) {
		if (std::none_of(g_last.begin(), g_last.end(),
		                 [&](const Pass& pass) { return pass.pixel_shader == shader; })) {
			result.emplace_back(Pass {shader}, true);
		}
	}
	return result;
}

} // namespace Common::PassInspector

#endif /* COMMON_PASS_INSPECTOR_H */
