// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "libs/audio.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Audio {

struct SpatialAttributes {
	AudioOut2::AudioOut2Position position {};
	float                        spread     = 0;
	uint32_t                     ambisonics = UINT32_MAX;

	bool SetPosition(const void* value, size_t size);
	bool SetSpread(const void* value, size_t size);
	bool SetAmbisonics(const void* value, size_t size, bool allow_acn);
};

struct SpatialObject {
	SpatialAttributes    attributes;
	float                gain     = 1;
	bool                 is_float = true;
	std::vector<uint8_t> pcm;
};

// One loopback context and one SDL submission handle per object bus.
class SpatialRenderer {
public:
	// CapyPS5: `debug.capy.audio2_openal 1` renders objects through this renderer; 0 (default)
	// keeps the stereo mix of object ports.
	static bool                             Enabled();
	static std::unique_ptr<SpatialRenderer> Open(uint32_t frames, uint32_t max_objects);
	~SpatialRenderer();
	int  OutputHandle() const { return m_handle; }
	bool Render(std::span<const SpatialObject> objects, std::vector<float>& output,
	            bool include_silence = false);

private:
	SpatialRenderer();
	struct Impl;
	std::unique_ptr<Impl> m;
	int                   m_handle = 0;
};

} // namespace Libs::Audio
