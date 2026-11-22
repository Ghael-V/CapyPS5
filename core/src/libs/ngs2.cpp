// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "libs/ngs2.h"

#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/ajm/atrac9_decoder.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "cpu/cpuBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <magic_enum.hpp>
#include <memory>
#include <numbers>
#include <numeric>
#include <string_view>
#include <tuple>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "libatrac9.h"

namespace Libs::Audio {

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

constexpr int32_t NGS2_ERROR_INVALID_SYSTEM_HANDLE =
    static_cast<int32_t>(0x804a8201u);
constexpr int32_t NGS2_ERROR_INVALID_RACK_HANDLE =
    static_cast<int32_t>(0x804a8202u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS =
    static_cast<int32_t>(0x804a8010u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_SIZE =
    static_cast<int32_t>(0x804a8011u);
constexpr int32_t NGS2_ERROR_INVALID_NUM_CHANNELS =
    static_cast<int32_t>(0x804a8052u);
constexpr int32_t NGS2_ERROR_FULL_WAVEFORM_BLOCKS =
    static_cast<int32_t>(0x804a8054u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS =
    static_cast<int32_t>(0x804a8058u);
constexpr int32_t NGS2_ERROR_INVALID_FILTER_TYPE =
    static_cast<int32_t>(0x804a8060u);
constexpr int32_t NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS =
    static_cast<int32_t>(0x804a8092u);
constexpr int32_t NGS2_ERROR_INVALID_PEAK_METER_DETECT_TYPE =
    static_cast<int32_t>(0x804a8093u);
constexpr int32_t NGS2_ERROR_INVALID_MATRIX_TYPE =
    static_cast<int32_t>(0x804a809au);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_INDEX =
    static_cast<int32_t>(0x804a8305u);
constexpr int32_t NGS2_ERROR_INVALID_MODULE_LOCATION =
    static_cast<int32_t>(0x804a8308u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_PARAM_ID =
    static_cast<int32_t>(0x804a835au);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE =
    static_cast<int32_t>(0x804a8361u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_CONTROL_ID =
    static_cast<int32_t>(0x804a8362u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_DATA =
    static_cast<int32_t>(0x804a8430u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_FORMAT =
    static_cast<int32_t>(0x804a8431u);
constexpr int32_t NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT =
    static_cast<int32_t>(0x804a8432u);

constexpr uint32_t NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;

struct Ngs2SystemOption {
	size_t    size                     = 0;
	char      name[64]                 = {};
	uintptr_t job_scheduler_options[4] = {};
	uint32_t  flags                    = 0;
	uint32_t  max_grain_samples        = 0;
	uint32_t  num_grain_samples        = 0;
	uint32_t  sample_rate              = 0;
	uint32_t  max_voice_channels       = 0;
	uint32_t  reserved[5]              = {};
};

struct Ngs2RackOption {
	size_t   size                   = 0;
	char     name[64]               = {};
	uint32_t flags                  = 0;
	uint32_t max_grain_samples      = 0;
	uint32_t max_voices             = 0;
	uint32_t max_input_delay_blocks = 0;
	uint32_t max_matrices           = 0;
	uint32_t max_ports              = 0;
	uint32_t max_voice_channels     = 0;
	uint32_t max_output_channels    = 0;
	uint32_t reserved[18]           = {};
};

struct Ngs2MasteringRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SubmixerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       max_envelope_points   = 0;
	uint32_t       max_filters           = 0;
	uint32_t       max_inputs            = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SamplerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channel_works        = 0;
	uint32_t       max_codec_caches         = 0;
	uint32_t       max_waveform_blocks      = 0;
	uint32_t       max_envelope_points      = 0;
	uint32_t       max_filters              = 0;
	uint32_t       max_atrac9_decoders      = 0;
	uint32_t       max_atrac9_channel_works = 0;
	uint32_t       max_ajm_atrac9_decoders  = 0;
	uint32_t       num_peak_meter_blocks    = 0;
};

struct Ngs2ReverbRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels = 0;
	uint32_t       reverb_size  = 0;
};

struct Ngs2CustomModuleOption {
	uint32_t size = 0;
};

struct Ngs2CustomRackModuleInfo {
	const Ngs2CustomModuleOption* option           = nullptr;
	uint32_t                      module_id        = 0;
	uint32_t                      source_buffer_id = 0;
	uint32_t                      extra_buffer_id  = 0;
	uint32_t                      dest_buffer_id   = 0;
	uint32_t                      state_offset     = 0;
	uint32_t                      state_size       = 0;
	uint32_t                      reserved         = 0;
	uint32_t                      reserved2        = 0;
};

struct Ngs2CustomRackPortInfo {
	uint32_t source_buffer_id = 0;
	uint32_t reserved         = 0;
};

struct Ngs2CustomRackOption {
	Ngs2RackOption           rack_option;
	uint32_t                 state_size  = 0;
	uint32_t                 num_buffers = 0;
	uint32_t                 num_modules = 0;
	uint32_t                 reserved    = 0;
	Ngs2CustomRackModuleInfo module[24];
	Ngs2CustomRackPortInfo   port[16];
};

struct Ngs2CustomSubmixerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomMasteringRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomSamplerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channel_works        = 0;
	uint32_t             max_waveform_blocks      = 0;
	uint32_t             max_atrac9_decoders      = 0;
	uint32_t             max_atrac9_channel_works = 0;
	uint32_t             max_ajm_atrac9_decoders  = 0;
	uint32_t             max_codec_caches         = 0;
};

union Ngs2RackOptionUnion {
	Ngs2RackOption                common;
	Ngs2SamplerRackOption         sampler;
	Ngs2MasteringRackOption       mastering;
	Ngs2SubmixerRackOption        submixer;
	Ngs2ReverbRackOption          reverb;
	Ngs2CustomSubmixerRackOption  custom_submixer;
	Ngs2CustomMasteringRackOption custom_mastering;
	Ngs2CustomSamplerRackOption   custom_sampler;
};

struct Ngs2ContextBufferInfo {
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

struct Ngs2SystemInfo {
	char                  name[64]      = {};
	uintptr_t             system_handle = 0;
	Ngs2ContextBufferInfo buffer_info;
	uint32_t              uid               = 0;
	uint32_t              min_grain_samples = 0;
	uint32_t              max_grain_samples = 0;
	uint32_t              state_flags       = 0;
	uint32_t              rack_count        = 0;
	float                 last_render_ratio = 0.0f;
	uint64_t              last_render_tick  = 0;
	uint64_t              render_count      = 0;
	uint32_t              sample_rate       = 0;
	uint32_t              num_grain_samples = 0;
};

struct Ngs2RenderBufferInfo {
	void*    buffer        = nullptr;
	size_t   buffer_size   = 0;
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
};

struct Ngs2WaveformFormat {
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
	uint32_t sample_rate   = 0;
	uint32_t config_data   = 0;
	uint32_t frame_offset  = 0;
	uint32_t frame_margin  = 0;
};

struct Ngs2WaveformBlock {
	uintptr_t data_offset      = 0;
	size_t    data_size        = 0;
	uint32_t  num_repeats      = 0;
	uint32_t  num_skip_samples = 0;
	uint32_t  num_samples      = 0;
	uint32_t  reserved         = 0;
	uintptr_t user_data        = 0;
};

// A block without skip or samples only carries data for the samples of the blocks before it.
static bool Ngs2IsDataOnly(const Ngs2WaveformBlock& block) {
	return block.num_skip_samples == 0 && block.num_samples == 0;
}

struct Ngs2WaveformInfo {
	Ngs2WaveformFormat format;
	uint32_t           data_offset              = 0;
	uint32_t           data_size                = 0;
	uint32_t           loop_begin_position      = 0;
	uint32_t           loop_end_position        = 0;
	uint32_t           num_samples              = 0;
	uint32_t           audio_unit_size          = 0;
	uint32_t           num_audio_unit_samples   = 0;
	uint32_t           num_audio_unit_per_frame = 0;
	uint32_t           audio_frame_size         = 0;
	uint32_t           num_audio_frame_samples  = 0;
	uint32_t           num_delay_samples        = 0;
	uint32_t           num_blocks               = 0;
	Ngs2WaveformBlock  blocks[4];
};

struct Ngs2PanParam {
	float angle     = 0.0f;
	float distance  = 0.0f;
	float fbw_level = 0.0f;
	float lfe_level = 0.0f;
};

struct Ngs2PanWork {
	float    speaker_angles[8] = {};
	float    unit_angle        = 0.0f;
	uint32_t num_speakers      = 0;
};

struct Ngs2GeomVector {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Ngs2GeomCone {
	float inner_level = 0.0f;
	float inner_angle = 0.0f;
	float outer_level = 0.0f;
	float outer_angle = 0.0f;
};

struct Ngs2GeomRolloff {
	uint32_t model              = 0;
	float    max_distance       = 0.0f;
	float    rolloff_factor     = 0.0f;
	float    reference_distance = 0.0f;
};

struct Ngs2GeomListenerParam {
	Ngs2GeomVector position;
	Ngs2GeomVector orient_front;
	Ngs2GeomVector orient_up;
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       reserved[2] = {};
};

struct Ngs2GeomListenerWork {
	float          matrix[4][4] = {};
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       coordinate  = 0;
	uint32_t       reserved[3] = {};
};

struct Ngs2GeomSourceParam {
	Ngs2GeomVector  position;
	Ngs2GeomVector  velocity;
	Ngs2GeomVector  direction;
	Ngs2GeomCone    cone;
	Ngs2GeomRolloff rolloff;
	float           doppler_factor = 0.0f;
	float           fbw_level      = 0.0f;
	float           lfe_level      = 0.0f;
	float           max_level      = 0.0f;
	float           min_level      = 0.0f;
	float           radius         = 0.0f;
	uint32_t        num_speakers   = 0;
	uint32_t        matrix_format  = 0;
	uint32_t        reserved[2]    = {};
};

struct Ngs2GeomA3dAttribute {
	Ngs2GeomVector position;
	float          volume      = 0.0f;
	uint32_t       reserved[4] = {};
};

struct Ngs2GeomAttribute {
	float                pitch_ratio = 0.0f;
	float                level[64]   = {};
	Ngs2GeomA3dAttribute a3d_attrib;
	uint32_t             reserved[4] = {};
};

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator {
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};

// The lock a game holds around a batch of calls; rendering and voice calls do not take it. Its
// mutex is held only inside Lock and Unlock, so a locked system or rack can be destroyed.
struct Ngs2UserLock {
	Common::Mutex   mutex;
	Common::CondVar released;
	int             owner = 0;
	uint32_t        depth = 0;

	void Lock() {
		const int         self = Common::Thread::GetThreadIdUnique();
		Common::LockGuard lock(mutex);
		while (depth != 0 && owner != self) {
			released.Wait(&mutex);
		}
		owner = self;
		depth++;
	}

	// An unlock by a thread that does not hold the lock is ignored.
	void Unlock() {
		Common::LockGuard lock(mutex);
		if (depth == 0 || owner != Common::Thread::GetThreadIdUnique()) {
			return;
		}
		if (--depth == 0) {
			released.Signal();
		}
	}
};

struct Ngs2Internal {
	Ngs2SystemOption      option;
	Ngs2ContextBufferInfo buffer_info;
	Ngs2BufferAllocator   allocator;
	Ngs2Internal*         next         = nullptr;
	uint64_t              render_count = 0;
	uint32_t              uid          = 0;
	Common::Mutex         mutex;
	Ngs2UserLock          user_lock;
};

enum class Ngs2RackType {
	Sampler,
	Submixer,
	Mastering,
	Reverb,
	CustomSubmixer,
	CustomMastering,
	CustomSampler,
};

struct Ngs2UserFxProcessContext {
	float**                  channels;
	std::array<uintptr_t, 3> user_data;
	uint32_t                 flags;
	uint32_t                 num_channels;
	uint32_t                 grain_samples;
	uint32_t                 sample_rate;
};

struct Ngs2UserFx2Context {
	void*     common;
	void*     param;
	void*     work;
	uintptr_t user_data;
	uint32_t  max_voices;
	uint32_t  voice_index;
	uint64_t  reserved[4] {};
};

struct Ngs2UserFx2ProcessContext {
	float**     channels;
	void*       common;
	const void* param;
	void*       work;
	void*       state;
	uintptr_t   user_data;
	uint32_t    flags;
	uint32_t    input_channels;
	uint32_t    output_channels;
	uint32_t    grain_samples;
	uint32_t    sample_rate;
	uint32_t    reserved {};
	uint64_t    reserved2[4] {};
};

struct Ngs2UserFx2Option {
	Ngs2CustomModuleOption header;
	int                    KYTY_SYSV_ABI (*setup)(Ngs2UserFx2Context*);
	int                    KYTY_SYSV_ABI (*cleanup)(Ngs2UserFx2Context*);
	uintptr_t              control;
	int                    KYTY_SYSV_ABI (*process)(Ngs2UserFx2ProcessContext*);
	size_t                 common_size;
	size_t                 param_size;
	size_t                 work_size;
	uintptr_t              user_data;
};

static bool Ngs2RackIsCustom(Ngs2RackType type) {
	switch (type) {
		case Ngs2RackType::CustomSubmixer:
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: return true;
		default: return false;
	}
}

struct Ngs2RackInternal {
	Ngs2Internal*                        ngs  = nullptr;
	Ngs2RackInternal*                    next = nullptr;
	Ngs2RackType                         type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion                  option;
	Ngs2ContextBufferInfo                buffer_info;
	Ngs2BufferAllocator                  allocator;
	std::array<std::vector<uint8_t>, 24> common;
	std::array<Ngs2UserFx2Option, 24>    fx {};
	Ngs2UserLock                         user_lock;

	[[nodiscard]] uint32_t NumBuffers() const {
		return Ngs2RackIsCustom(type) ? option.custom_sampler.custom_rack_option.num_buffers : 1;
	}

	[[nodiscard]] uint32_t PortBufferId(size_t port) const {
		const auto& custom = option.custom_sampler.custom_rack_option;
		return Ngs2RackIsCustom(type) ? custom.port[port].source_buffer_id : 0;
	}
};

enum class Ngs2VoicePlayState : uint32_t { Empty = 0, Playing = 3, Paused = 5, Stopped = 0xb };

// A filter: channel_mask bits mark the channels that pass unfiltered; DIRECT (type 11) takes its
// coefficients verbatim, the other types design them from fc, q and level.
struct Ngs2FilterParam {
	uint64_t channel_mask;
	uint32_t type;
	union {
		struct {
			float i0, i1, i2, o1, o2;
		} direct;
		struct {
			float    fc, q, level;
			uint32_t reserved, reserved2;
		} fcq;
	} param;
};
static_assert(sizeof(Ngs2FilterParam) == 32);

struct Ngs2SamplerFilterParam {
	uint32_t        index;
	uint32_t        location;
	Ngs2FilterParam filter;
	uint32_t        reserved3;
};
static_assert(sizeof(Ngs2SamplerFilterParam) == 48);

// A biquad in direct form I: y = i0 * x + i1 * x1 + i2 * x2 + o1 * y1 + o2 * y2, in float.
struct Ngs2SamplerFilter {
	uint32_t location     = 0;
	uint32_t type         = 0;
	uint64_t channel_mask = 0;
	float    i0 = 1, i1 = 0, i2 = 0, o1 = 0, o2 = 0;
	// Per channel {x1, x2, y1, y2}; empty until the filter is set.
	std::vector<std::array<float, 4>> history;

	// Designs the filter at `rate`. The cutoff is clamped to 0.0001..0.499 of the rate and Q to
	// 0.0001..32; only the shelves and peaking read the level, a linear gain.
	void Design(const Ngs2FilterParam& param, uint32_t rate) {
		type         = param.type;
		channel_mask = param.channel_mask;
		if (type == 11) {
			const auto& direct = param.param.direct;
			std::tie(i0, i1, i2, o1, o2) =
			    std::tie(direct.i0, direct.i1, direct.i2, direct.o1, direct.o2);
			return;
		}
		const auto& fcq   = param.param.fcq;
		const float w     = std::clamp(fcq.fc / static_cast<float>(rate), 1e-4f, 0.499f) * 2 *
		                    std::numbers::pi_v<float>;
		const float q     = std::clamp(fcq.q, 1e-4f, 32.0f);
		const float s     = std::sin(w);
		const float c     = std::cos(w);
		const float alpha = s / (2 * q);
		const float gain  = std::sqrt(std::max(1e-4f, fcq.level));
		float       b0 = 0, b1 = 0, b2 = 0, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
		switch (type) {
			case 1: // Low-pass.
				b0 = b2 = (1 - c) * 0.5f;
				b1      = 1 - c;
				break;
			case 2: // High-pass.
				b0 = b2 = (1 + c) * 0.5f;
				b1      = -(1 + c);
				break;
			case 3: // Band-pass with a peak gain of Q.
				b0 = s * 0.5f;
				b2 = -b0;
				break;
			case 4: // Band-pass with a unity peak.
				b0 = alpha;
				b2 = -alpha;
				break;
			case 5: // Notch.
				b0 = b2 = 1;
				b1      = a1;
				break;
			case 6:
			case 7: {
				// The high shelf is the low shelf mirrored around half the rate.
				const float sign = type == 6 ? 1.0f : -1.0f;
				const float e    = sign * c;
				const float k    = std::sqrt(gain) / q * s;
				b0               = gain * ((gain + 1) - (gain - 1) * e + k);
				b1               = sign * 2 * gain * ((gain - 1) - (gain + 1) * e);
				b2               = gain * ((gain + 1) - (gain - 1) * e - k);
				a0               = (gain + 1) + (gain - 1) * e + k;
				a1               = -sign * 2 * ((gain - 1) + (gain + 1) * e);
				a2               = (gain + 1) + (gain - 1) * e - k;
				break;
			}
			case 8: // Peaking.
				b0 = 1 + alpha * gain;
				b1 = a1;
				b2 = 1 - alpha * gain;
				a0 = 1 + alpha / gain;
				a2 = 1 - alpha / gain;
				break;
			case 9: // One-pole low-pass.
				b0 = b1 = s;
				a0      = (s + c) + 1;
				a1      = (s - c) - 1;
				a2      = 0;
				break;
			default: // One-pole high-pass (10); OFF (0) bypasses every channel.
				b0 = 1 + c;
				b1 = -b0;
				a0 = s + (1 + c);
				a1 = s - (1 + c);
				a2 = 0;
		}
		const float inverse = 1 / a0;
		i0                  = b0 * inverse;
		i1                  = b1 * inverse;
		i2                  = b2 * inverse;
		o1                  = -a1 * inverse;
		o2                  = -a2 * inverse;
	}

	bool Bypasses(size_t channel) const {
		return type == 0 || (channel < 64 && (channel_mask & (uint64_t {1} << channel)) != 0);
	}

	// Whether the filter has history left; a bypassed channel's follows its input to zero.
	bool HasTail() const {
		return std::ranges::any_of(history,
		                           [](const auto& h) { return h != std::array<float, 4> {}; });
	}

	// Filters channel-major samples in place. A bypassed channel, every one for OFF, passes
	// through, its history following the input.
	void Process(float* samples, uint32_t grain) {
		for (size_t c = 0; c < history.size(); ++c) {
			auto* x                = samples + c * grain;
			auto& [x1, x2, y1, y2] = history[c];
			if (Bypasses(c)) {
				x2 = y2 = grain > 1 ? x[grain - 2] : x1;
				x1 = y1 = x[grain - 1];
				continue;
			}
			for (uint32_t i = 0; i < grain; ++i) {
				float y = i0 * x[i] + i1 * x1 + i2 * x2 + o1 * y1 + o2 * y2;
				// A decayed tail ends at zero.
				if (std::abs(y) < 1e-20f) {
					y = 0;
				}
				x2   = x1;
				x1   = x[i];
				y2   = y1;
				y1   = y;
				x[i] = y;
			}
		}
	}
};

// The channel converter keeps one level field per input count and field output count (1, 2, 4, 6
// or 8), in this order; an output count reads the first rows of the smallest field with at least
// as many channels. Levels are dst-major rows of one level per input: 0, 1, h = 0.70710677,
// q = 0.5, e = 0.35355339.
struct Ngs2MasteringField {
	uint32_t         inputs;
	uint32_t         outputs;
	std::string_view levels;
};
// clang-format off
constexpr std::array<Ngs2MasteringField, 40> NGS2_MASTERING_FIELDS = {{
    {1, 1, "1"},
    {1, 2, "hh"},
    {1, 4, "hh00"},
    {1, 6, "001000"},
    {1, 8, "00100000"},
    {2, 1, "hh"},
    {2, 2, "10" "01"},
    {2, 4, "10" "01" "00" "00"},
    {2, 6, "10" "01" "00" "00" "00" "00"},
    {2, 8, "10" "01" "00" "00" "00" "00" "00" "00"},
    {3, 1, "hh1"},
    {3, 2, "10h" "01h"},
    {3, 4, "10h" "01h" "000" "000"},
    {3, 6, "100" "010" "001" "000" "000" "000"},
    {3, 8, "100" "010" "001" "000" "000" "000" "000" "000"},
    {4, 1, "hhqq"},
    {4, 2, "10h0" "010h"},
    {4, 4, "1000" "0100" "0010" "0001"},
    {4, 6, "1000" "0100" "0000" "0000" "0010" "0001"},
    {4, 8, "1000" "0100" "0000" "0000" "0010" "0001" "0000" "0000"},
    {5, 1, "hh1qq"},
    {5, 2, "10hh0" "01h0h"},
    {5, 4, "10h00" "01h00" "00010" "00001"},
    {5, 6, "10000" "01000" "00100" "00000" "00010" "00001"},
    {5, 8, "10000" "01000" "00100" "00000" "00010" "00001" "00000" "00000"},
    {6, 1, "hh10qq"},
    {6, 2, "10h0h0" "01h00h"},
    {6, 4, "10h000" "01h000" "000010" "000001"},
    {6, 6, "100000" "010000" "001000" "000100" "000010" "000001"},
    {6, 8, "100000" "010000" "001000" "000100" "000010" "000001" "000000" "000000"},
    {7, 1, "hh10qqe"},
    {7, 2, "10h0h0q" "01h00hq"},
    {7, 4, "10h0000" "01h0000" "000010h" "000001h"},
    {7, 6, "1000000" "0100000" "0010000" "0001000" "000010h" "000001h"},
    {7, 8, "1000000" "0100000" "0010000" "0001000" "0000100" "0000010" "000000h" "000000h"},
    {8, 1, "hh10hhhh"},
    {8, 2, "10h0h0h0" "01h00h0h"},
    {8, 4, "10000000" "01000000" "00001010" "00000101"},
    {8, 6, "10000000" "01000000" "00100000" "00010000" "00001010" "00000101"},
    {8, 8, "10000000" "01000000" "00100000" "00010000" "00001000" "00000100" "00000010" "00000001"},
}};
// clang-format on

// Offset of the levels that convert `inputs` into `outputs` channels.
static constexpr size_t Ngs2MasteringLevelsOffset(uint32_t inputs, uint32_t outputs) {
	size_t offset = 0;
	for (const auto& field: NGS2_MASTERING_FIELDS) {
		if (field.inputs == inputs && field.outputs >= outputs) {
			break;
		}
		offset += field.levels.size();
	}
	return offset;
}

constexpr auto NGS2_MASTERING_DEFAULT_LEVELS = [] {
	constexpr float h = std::numbers::sqrt2_v<float> / 2;

	std::array<float, Ngs2MasteringLevelsOffset(8, 8) + 8 * 8> levels {};
	for (size_t n = 0; const auto& field: NGS2_MASTERING_FIELDS) {
		for (const char level: field.levels) {
			switch (level) {
				case '1': levels[n] = 1.0f; break;
				case 'h': levels[n] = h; break;
				case 'q': levels[n] = 0.5f; break;
				case 'e': levels[n] = h / 2; break;
				default: break;
			}
			++n;
		}
	}
	return levels;
}();

struct Ngs2VoiceInternal {
	struct Port {
		Ngs2VoiceInternal* dest   = nullptr;
		uint32_t           input  = 0;
		float              volume = 1.0f;
		int32_t            matrix = -1;
	};
	struct Block {
		const uint8_t*    data;
		Ngs2WaveformBlock info;
		size_t            data_cursor    = 0;
		uint32_t          repeated_count = 0;
	};
	struct Module {
		std::vector<uint8_t>     param, work, state;
		std::vector<float>       channel_matrix;
		void                     KYTY_SYSV_ABI (*handler)(Ngs2UserFxProcessContext*) = nullptr;
		std::array<uintptr_t, 3> user_data {};
		uint32_t                 flags          = 1;
		bool                     control_warned = false;
		bool                     render_warned  = false;
	};
	// Mastering voice module state.
	struct Mastering {
		std::array<float, NGS2_MASTERING_DEFAULT_LEVELS.size()> levels =
		    NGS2_MASTERING_DEFAULT_LEVELS;
		// The converter levels of the previous grain and their shape; inputs 0 skips the ramp.
		std::array<float, 64> previous_levels {};
		uint32_t              previous_inputs  = 0;
		uint32_t              previous_outputs = 0;
		// LFE filter on channel 3: two sections y = b0 * x + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2,
		// (b0, b1, b2) = gain * (1, 2, 1). An LFE control enables the stage until Setup; the first
		// grain after a grain without it, or after PLAY, starts from zero history. A cleared enable
		// flag leaves channel 3 and the history as they are.
		struct LfeSection {
			float gain = 0.0f, a1 = 0.0f, a2 = 0.0f;
			struct History {
				float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
			} history;
		};
		std::array<LfeSection, 2> lfe {};
		bool                      lfe_enable_flag = false;
		bool                      lfe_enabled     = false;
		// Limiter: each block of 32 samples takes the peak of every channel, and the gain moves
		// toward threshold / peak above the threshold, else 1, with the attack coefficient while
		// above that target and the release coefficient otherwise. A limiter control enables the
		// stage until Setup; a grain without it, a cleared enable flag or PLAY restore unity gain.
		// The state is the last block's peak and the gain after it, {0, 1} with a cleared enable
		// flag and zero until the stage renders after Setup.
		float                limiter_threshold = 0.0f;
		float                limiter_attack    = 0.0f;
		float                limiter_release   = 0.0f;
		float                limiter_gain      = 1.0f;
		std::array<float, 2> limiter_state {};
		bool                 limiter_enable_flag = false;
		bool                 limiter_enabled     = false;
		// Full band and LFE gains. A gain control enables the gain until Setup; the first grain
		// after a grain without it, or after PLAY, starts at the new gains.
		std::array<float, 2> gain {};
		std::array<float, 2> previous_gain {};
		bool                 gain_enabled  = false;
		bool                 gain_rendered = false;
		// Peak meters on the N channels before the converter and after the gain, sharing a detect
		// type and a window of the rack's blocks. A peak meter control enables both until Setup;
		// a detect type change, a grain without them and PLAY restart the window. Each grain
		// writes its peak (detect type 1) or energy (2) per channel into the window's next block,
		// and the meters report the window's peak or RMS per channel, none for detect type 0 or
		// after Setup. Mode 0 means no window. Each channel keeps the input then the output
		// meter's window.
		std::vector<float>                   meter_blocks;
		std::array<std::array<float, 37>, 2> meter_peaks {};
		uint32_t                             meter_detect_type = 0;
		uint32_t                             meter_mode        = 0;
		uint32_t                             meter_index       = 0;
		uint32_t                             meter_count       = 0;
		uint32_t                             meter_channels    = 0;
		bool                                 meters_enabled    = false;
	};
	Ngs2VoicePlayState              state          = Ngs2VoicePlayState::Empty;
	uint32_t                        state_flags    = 0;
	Ngs2RackInternal*               rack           = nullptr;
	uintptr_t                       callback       = 0;
	uintptr_t                       callback_data  = 0;
	uint32_t                        callback_flags = 0;
	std::deque<Block>               blocks;
	std::vector<Port>               ports;
	std::vector<std::vector<float>> matrices;
	std::vector<Module>             modules;
	std::vector<float>              samples;
	std::vector<float>              mix_samples;
	uint32_t                        channels        = 0;
	uint32_t                        output_id       = 0;
	std::unique_ptr<Mastering>      mastering;
	bool                            rendering       = false;
	bool                            rendered        = false;
	bool                            has_samples     = false;

	std::unique_ptr<Ajm::AjmAt9Decoder> decoder;
	std::vector<float>                  decoded_frame;
	std::vector<float>                  next_decoded_frame;
	std::vector<uint8_t>                compressed_input;
	uint32_t                            frame_cursor       = 0;
	uint32_t                            frame_samples      = 0;
	uint32_t                            next_frame_samples = 0;
	bool                                accepts_blocks     = true;
	uint32_t                            sample_rate        = 0;
	uint64_t                            sample_phase       = 0;
	uint64_t                            sample_step        = 0;
	uint64_t                            decoded_samples    = 0;
	uint64_t                            decoded_bytes      = 0;
	uint64_t                            source_generation  = 0;
	uint32_t                            duration_remaining = 0;
	uint32_t                            skip_remaining     = 0;
	const uint8_t*                      waveform_end       = nullptr;
	std::vector<Ngs2SamplerFilter>      filters;

	const uint8_t* WaveformData() const {
		return blocks.empty() ? waveform_end : blocks.front().data + blocks.front().data_cursor;
	}

	// Custom racks keep num_buffers consecutive channel-major buffers; other racks keep one.
	void ClearSamples(uint32_t grain) {
		samples.assign(size_t(grain) * channels * rack->NumBuffers(), 0.0f);
	}

	float* Buffer(uint32_t id, uint32_t grain) {
		return samples.data() + size_t(id) * grain * channels;
	}

	void MixChannels(const std::vector<float>& matrix, uint32_t buffer, uint32_t grain) {
		if (matrix.empty()) {
			return;
		}
		EXIT_NOT_IMPLEMENTED(channels > 37);
		auto* input = Buffer(buffer, grain);
		mix_samples.assign(size_t(channels) * grain, 0.0f);
		for (uint32_t dst = 0; dst < channels; ++dst) {
			for (uint32_t src = 0; src < channels; ++src) {
				const float level = matrix[dst * 37 + src];
				if (level == 0.0f) {
					continue;
				}
				for (uint32_t i = 0; i < grain; ++i) {
					mix_samples[dst * grain + i] += input[src * grain + i] * level;
				}
			}
		}
		std::copy(mix_samples.begin(), mix_samples.end(), input);
	}

	// Runs the filters at `location`, in index order, on channel-major samples.
	void RunFilters(uint32_t location, float* data, uint32_t grain) {
		for (auto& filter: filters) {
			if (filter.location == location) {
				filter.Process(data, grain);
			}
		}
	}

	void ResetSetupState() {
		SetEvent(4);
		std::ranges::fill(ports, Port {});
		for (auto& matrix: matrices) {
			matrix.clear();
		}
		filters.clear();
	}

	void ClearWaveformBlocks() {
		++source_generation;
		blocks.clear();
		sample_phase       = 0;
		duration_remaining = 0;
		skip_remaining     = 0;
		compressed_input.clear();
		frame_cursor       = 0;
		frame_samples      = 0;
		next_frame_samples = 0;
		waveform_end       = nullptr;
	}

	void SetupSampler(const Ngs2WaveformFormat& format) {
		const bool deferred = format.waveform_type == 0 && format.num_channels == 0 &&
		                      format.sample_rate == 0 && format.config_data == 0 &&
		                      format.frame_margin == 0 && format.frame_offset == 0;
		EXIT_NOT_IMPLEMENTED(!deferred &&
		                     (format.frame_margin != 0 || format.frame_offset != 0 ||
		                      format.sample_rate == 0 || format.num_channels == 0));
		ResetSetupState();
		ClearWaveformBlocks();
		channels        = format.num_channels;
		sample_rate     = format.sample_rate;
		sample_step     = uint64_t(sample_rate) << 32u;
		decoded_samples = 0;
		decoded_bytes   = 0;
		decoder.reset();
		decoded_frame.clear();
		next_decoded_frame.clear();
		accepts_blocks = true;
		if (deferred) {
			return;
		}
		if (format.waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
			decoder = std::make_unique<Ajm::AjmAt9Decoder>(channels, format.sample_rate,
			                                               Ajm::AjmSampleEncoding::Float, 0);
			const std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config = {
			    static_cast<uint8_t>(format.config_data >> 24u),
			    static_cast<uint8_t>(format.config_data >> 16u),
			    static_cast<uint8_t>(format.config_data >> 8u),
			    static_cast<uint8_t>(format.config_data)};
			const auto result = decoder->Initialize(config.data(), config.size());
			EXIT_NOT_IMPLEMENTED(result.result != OK);
			EXIT_NOT_IMPLEMENTED(result.format.channel_num != channels ||
			                     result.format.sampling_frequency != format.sample_rate);
			Ajm::AjmSidebandDecAt9CodecInfo info {};
			decoder->WriteCodecInfo(&info, sizeof(info), result);
			EXIT_NOT_IMPLEMENTED(info.frame_samples > std::numeric_limits<uint16_t>::max());
			decoded_frame.resize(info.frame_samples * channels);
			compressed_input.reserve(info.super_frame_size);
		} else {
			EXIT_NOT_IMPLEMENTED(format.waveform_type != 0x12);
		}
	}

	int AddWaveformBlocks(const uint8_t* data, uint32_t flags, uint32_t count,
	                      const Ngs2WaveformBlock* waveform_blocks) {
		const auto capacity = rack->type == Ngs2RackType::Sampler
		                          ? rack->option.sampler.max_waveform_blocks
		                          : rack->option.custom_sampler.max_waveform_blocks;
		const bool clear    = (flags & 4u) != 0;
		const bool cancel   = (flags & 8u) != 0;
		// Only CLEAR reopens a list that the previous addition closed.
		if (!clear && !accepts_blocks) {
			return NGS2_ERROR_FULL_WAVEFORM_BLOCKS;
		}
		// The capacity counts added blocks before they join; CANCEL keeps room for the front block.
		size_t kept = cancel ? 1 : blocks.size();
		if (clear) {
			kept = 0;
		}
		if (kept + count > capacity) {
			return NGS2_ERROR_FULL_WAVEFORM_BLOCKS;
		}
		if (clear) {
			ClearWaveformBlocks();
		} else if (cancel) {
			// CANCEL drops every block behind the front one, started or not, without a callback.
			blocks.resize(std::min<size_t>(blocks.size(), 1));
		}
		const bool only_data = (flags & 2u) != 0;
		const bool join      = (flags & 0x10u) == 0;
		for (uint32_t i = 0; i < count; ++i) {
			auto info = waveform_blocks[i];
			if (only_data) {
				info.num_repeats      = 0;
				info.num_skip_samples = 0;
				info.num_samples      = 0;
			}
			const auto* block_data = data + info.data_offset;
			// A data-only block that does not repeat extends a contiguous last block of the same
			// user data.
			if (join && !blocks.empty() && Ngs2IsDataOnly(info) && info.num_repeats == 0 &&
			    blocks.back().data + blocks.back().info.data_size == block_data &&
			    blocks.back().info.user_data == info.user_data) {
				blocks.back().info.data_size += info.data_size;
			} else {
				blocks.push_back({block_data, info});
			}
		}
		accepts_blocks = (flags & 1u) != 0;
		return OK;
	}

	void SetMatrix(uint32_t index, const float* levels, uint32_t count) {
		EXIT_NOT_IMPLEMENTED(index >= matrices.size());
		matrices[index].assign(levels, levels + count);
	}
	void SetVolume(uint32_t index, float volume) {
		EXIT_NOT_IMPLEMENTED(index >= ports.size());
		ports[index].volume = volume;
	}
	void SetPortMatrix(uint32_t index, int32_t matrix) {
		EXIT_NOT_IMPLEMENTED(index >= ports.size());
		ports[index].matrix = matrix;
	}
	void SetEvent(uint32_t id) {
		switch (id) {
			case 1:
				if (state == Ngs2VoicePlayState::Empty || state == Ngs2VoicePlayState::Stopped) {
					state = Ngs2VoicePlayState::Playing;
					// Reserve the voice immediately; publish playback flags after rendering.
					state_flags |= 1;
				}
				// Every PLAY restarts the filters and mastering modules without a ramp or history.
				for (auto& filter: filters) {
					std::ranges::fill(filter.history, std::array<float, 4> {});
				}
				if (mastering != nullptr) {
					mastering->previous_inputs = 0;
					mastering->lfe[0].history  = {};
					mastering->lfe[1].history  = {};
					mastering->limiter_gain    = 1.0f;
					mastering->gain_rendered   = false;
					mastering->meter_mode      = 0;
				}
				break;
			case 2:
				if (state == Ngs2VoicePlayState::Playing || state == Ngs2VoicePlayState::Paused) {
					state = Ngs2VoicePlayState::Stopped;
				}
				break;
			case 4:
			case 8: state = Ngs2VoicePlayState::Empty; break;
			case 16:
				if (state == Ngs2VoicePlayState::Playing) {
					state = Ngs2VoicePlayState::Paused;
				}
				break;
			case 32:
				if (state == Ngs2VoicePlayState::Paused) {
					state = Ngs2VoicePlayState::Playing;
				}
				break;
			// PS4 games send events (such as 0) that change nothing here.
			default:
				Common::DebugFlags::NoteOnce("CapyPS5-Ngs2", 0x4e475332'00000000ull | id, "ignored voice event 0x%08x",
				                             id);
				break;
		}
	}
};

struct Ngs2VoiceParamHeader {
	uint16_t size;
	int16_t  next;
	uint32_t id;
};

struct Ngs2VoiceEventParam {
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2SubmixerVoiceSetupParam {
	Ngs2VoiceParamHeader header;
	uint32_t             num_io_channels;
	uint32_t             flags;
};
static_assert(sizeof(Ngs2SubmixerVoiceSetupParam) == 16);

struct Ngs2VoicePatchParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             dest_input_id;
	uintptr_t            dest_handle;
};

struct Ngs2VoiceMatrixLevelsParam {
	Ngs2VoiceParamHeader header;
	uint32_t             matrix_id;
	uint32_t             num_levels;
	const float*         levels;
};

struct Ngs2VoicePortMatrixParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	int32_t              matrix_id;
};

struct Ngs2VoicePortVolumeParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	float                level;
};

struct Ngs2VoicePortDelayParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             num_samples;
};

struct Ngs2VoiceCallbackParam {
	Ngs2VoiceParamHeader header;
	uintptr_t            callback;
	uintptr_t            callback_data;
	uint32_t             flags;
	uint32_t             reserved;
};

struct Ngs2VoiceState {
	uint32_t state_flags;
	int32_t  error_code;
};

struct Ngs2SubmixerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	float          compressor_height;
};

struct Ngs2MasteringVoiceState {
	Ngs2VoiceState voice_state;
	float          limiter_peak_level;
	float          limiter_press_level;
	float          input_peak_height[37];
	float          output_peak_height[37];
};

struct Ngs2CustomMasteringVoiceState {
	Ngs2VoiceState voice_state;
	uint32_t       reserved;
	uint32_t       reserved2;
};

struct Ngs2SamplerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};

struct Ngs2CustomSamplerVoiceState {
	Ngs2VoiceState voice_state;
	const void*    waveform_data;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	uint32_t       reserved;
	uint32_t       reserved2;
};

static Ngs2Internal*        g_ngs_list     = nullptr;
static Ngs2RackInternal*    g_racks_list   = nullptr;
static std::atomic_uint32_t g_next_ngs_uid = 1;
static Common::Mutex        g_racks_mutex;
// Guards g_ngs_list; no other lock is taken while it is held.
static Common::Mutex g_ngs_mutex;

static_assert(sizeof(Ngs2UserFxProcessContext) == 48);
static_assert(sizeof(Ngs2UserFx2Context) == 72);
static_assert(sizeof(Ngs2UserFx2ProcessContext) == 104);
static_assert(sizeof(Ngs2UserFx2Option) == 72);
static_assert(sizeof(Ngs2SystemOption) == 144);
static_assert(sizeof(Ngs2SystemInfo) == 184);
static_assert(sizeof(Ngs2RackOption) == 176);
static_assert(sizeof(Ngs2VoiceState) == 8);
static_assert(sizeof(Ngs2SubmixerVoiceState) == 20);
static_assert(sizeof(Ngs2MasteringVoiceState) == 0x138);
static_assert(sizeof(Ngs2CustomMasteringVoiceState) == 16);
static_assert(sizeof(Ngs2SamplerVoiceState) == 56);
static_assert(sizeof(Ngs2CustomSamplerVoiceState) == 48);
static_assert(sizeof(Ngs2WaveformFormat) == 24);
static_assert(offsetof(Ngs2WaveformFormat, frame_offset) == 16 &&
              offsetof(Ngs2WaveformFormat, frame_margin) == 20);
static_assert(sizeof(Ngs2WaveformBlock) == 40);
static_assert(sizeof(Ngs2WaveformInfo) == 232);

static uint32_t Ngs2GetStateFlags(const Ngs2VoiceInternal* voice) {
	return voice->state_flags;
}

static Ngs2SystemOption Ngs2DefaultSystemOption() {
	Ngs2SystemOption option {};
	option.size              = sizeof(Ngs2SystemOption);
	option.max_grain_samples = 512;
	option.num_grain_samples = 256;
	option.sample_rate       = 48000;
	return option;
}

// The system of racks whose system was destroyed. The racks stay valid handles that never render;
// their voice calls apply as usual, at the default sample rate.
static Ngs2Internal g_detached_system {.option = Ngs2DefaultSystemOption()};

// Unlinks and returns the list entry with this handle, or nullptr.
template <typename T>
static T* Ngs2Unlink(T*& list, uintptr_t handle) {
	auto** link = &list;
	while (*link != nullptr && reinterpret_cast<uintptr_t>(*link) != handle) {
		link = &(*link)->next;
	}
	auto* entry = *link;
	if (entry != nullptr) {
		*link = entry->next;
	}
	return entry;
}

int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = Ngs2DefaultSystemOption();
	return OK;
}

// The first SDK's system option (PS4 games): a 16-byte name, no job scheduler options, flags and
// grain/rate fields right after. Converted to the layout used here; PS5 options pass through.
static const Ngs2SystemOption* Ngs2NativeSystemOption(const Ngs2SystemOption* option, Ngs2SystemOption* out) {
	constexpr size_t Ps4FieldsEnd = 40;
	if (option == nullptr || option->size >= sizeof(Ngs2SystemOption) || option->size < Ps4FieldsEnd) {
		return option;
	}
	const auto* raw = reinterpret_cast<const uint8_t*>(option);
	*out            = {};
	out->size       = sizeof(Ngs2SystemOption);
	std::memcpy(out->name, raw + 8, 16);
	uint32_t fields[4] = {};
	std::memcpy(fields, raw + 24, sizeof(fields));
	out->flags             = fields[0];
	out->max_grain_samples = fields[1];
	out->num_grain_samples = fields[2];
	out->sample_rate       = fields[3];
	return out;
}

static Ngs2Internal* Ngs2CreateSystemInternal(const Ngs2SystemOption*      option,
                                              const Ngs2ContextBufferInfo* buffer_info) {
	auto* ngs = new (buffer_info->host_buffer) Ngs2Internal;

	ngs->option      = *option;
	ngs->buffer_info = *buffer_info;
	ngs->uid         = g_next_ngs_uid.fetch_add(1, std::memory_order_relaxed);

	Common::LockGuard lock(g_ngs_mutex);
	ngs->next  = g_ngs_list;
	g_ngs_list = ngs;

	return ngs;
}

static Ngs2Internal* Ngs2FindSystem(uintptr_t system_handle) {
	Common::LockGuard lock(g_ngs_mutex);
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next) {
		if (reinterpret_cast<uintptr_t>(ngs) == system_handle) {
			return ngs;
		}
	}
	return nullptr;
}

int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}
	Ngs2SystemOption native_option {};
	option = Ngs2NativeSystemOption(option, &native_option);

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	std::memset(buffer_info, 0, sizeof(Ngs2ContextBufferInfo));
	buffer_info->host_buffer_size = sizeof(Ngs2Internal);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size < sizeof(Ngs2Internal));

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}
	Ngs2SystemOption native_option {};
	option = Ngs2NativeSystemOption(option, &native_option);

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	auto* ngs = Ngs2CreateSystemInternal(option, buffer_info);

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

static void Ngs2FillDefaultRackOption(uint32_t rack_id, Ngs2RackOptionUnion* option) {
	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = {};

	switch (rack_id) {
		case 0x1000:
			option->sampler.rack_option.size                   = sizeof(Ngs2SamplerRackOption);
			option->sampler.rack_option.max_grain_samples      = 512;
			option->sampler.rack_option.max_voices             = 256;
			option->sampler.rack_option.max_input_delay_blocks = 0;
			option->sampler.rack_option.max_matrices           = 1;
			option->sampler.rack_option.max_ports              = 8;
			option->sampler.max_channel_works                  = 256;
			option->sampler.max_codec_caches                   = 32;
			option->sampler.max_waveform_blocks                = 4;
			option->sampler.max_envelope_points                = 4;
			option->sampler.max_filters                        = 8;
			option->sampler.max_atrac9_decoders                = 256;
			option->sampler.max_atrac9_channel_works           = 256;
			option->sampler.max_ajm_atrac9_decoders            = 0;
			option->sampler.num_peak_meter_blocks              = 8;
			break;
		case 0x2000:
			option->submixer.rack_option.size                   = sizeof(Ngs2SubmixerRackOption);
			option->submixer.rack_option.max_grain_samples      = 512;
			option->submixer.rack_option.max_voices             = 1;
			option->submixer.rack_option.max_input_delay_blocks = 1;
			option->submixer.rack_option.max_matrices           = 1;
			option->submixer.rack_option.max_ports              = 8;
			option->submixer.max_channels                       = 8;
			option->submixer.max_envelope_points                = 4;
			option->submixer.max_filters                        = 8;
			option->submixer.max_inputs                         = 1;
			option->submixer.num_peak_meter_blocks              = 8;
			break;
		case 0x2001:
			option->reverb.rack_option.size                   = sizeof(Ngs2ReverbRackOption);
			option->reverb.rack_option.max_grain_samples      = 512;
			option->reverb.rack_option.max_voices             = 1;
			option->reverb.rack_option.max_input_delay_blocks = 1;
			option->reverb.rack_option.max_matrices           = 1;
			option->reverb.rack_option.max_ports              = 8;
			option->reverb.max_channels                       = 8;
			option->reverb.reverb_size                        = 1;
			break;
		case 0x3000:
			option->mastering.rack_option.size                   = sizeof(Ngs2MasteringRackOption);
			option->mastering.rack_option.max_grain_samples      = 512;
			option->mastering.rack_option.max_voices             = 1;
			option->mastering.rack_option.max_input_delay_blocks = 1;
			option->mastering.rack_option.max_matrices           = 0;
			option->mastering.rack_option.max_ports              = 0;
			option->mastering.max_channels                       = 8;
			option->mastering.num_peak_meter_blocks              = 8;
			break;
		case 0x4002:
			// FIXME: Temporary PS5 progress fallback. This mirrors Prospero reset helper's
			// common custom-submixer defaults, but the custom module internals are still stubbed.
			option->custom_submixer.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSubmixerRackOption);
			option->custom_submixer.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_submixer.custom_rack_option.rack_option.max_voices             = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_input_delay_blocks = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_submixer.custom_rack_option.num_buffers                        = 1;
			option->custom_submixer.max_channels                                          = 8;
			option->custom_submixer.max_inputs                                            = 1;
			break;
		case 0x4001:
			option->custom_sampler.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSamplerRackOption);
			option->custom_sampler.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_sampler.custom_rack_option.rack_option.max_voices             = 256;
			option->custom_sampler.custom_rack_option.rack_option.max_input_delay_blocks = 0;
			option->custom_sampler.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_sampler.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_sampler.custom_rack_option.num_buffers                        = 1;
			option->custom_sampler.max_channel_works                                     = 256;
			option->custom_sampler.max_waveform_blocks                                   = 4;
			option->custom_sampler.max_atrac9_decoders                                   = 256;
			option->custom_sampler.max_atrac9_channel_works                              = 256;
			option->custom_sampler.max_ajm_atrac9_decoders                               = 0;
			option->custom_sampler.max_codec_caches                                      = 32;
			break;
		default: EXIT("unknown rack_id for default option: 0x%" PRIx32 "\n", rack_id);
	}
}

// PS4 games (backward compatibility) pass the first SDK's rack option: a 16-byte name, no voice or
// output channel counts and 128 bytes in all, followed by the rack's own fields. Converts it to the
// layout used here; PS5 options are larger and pass through untouched.
static const Ngs2RackOption* Ngs2NativeRackOption(const Ngs2RackOption* option, Ngs2RackOptionUnion* out) {
	constexpr size_t Ps4CommonSize = 128;
	if (option == nullptr || option->size >= sizeof(Ngs2RackOption) || option->size < Ps4CommonSize) {
		return option;
	}
	const auto* raw    = reinterpret_cast<const uint8_t*>(option);
	auto*       common = &out->common;
	std::memcpy(common->name, raw + 8, 16);
	uint32_t fields[6] = {};
	std::memcpy(fields, raw + 24, sizeof(fields));
	common->flags                  = fields[0];
	common->max_grain_samples      = fields[1];
	common->max_voices             = fields[2];
	common->max_input_delay_blocks = fields[3];
	common->max_matrices           = fields[4];
	common->max_ports              = fields[5];
	const size_t tail = std::min(option->size - Ps4CommonSize, sizeof(*out) - sizeof(Ngs2RackOption));
	std::memcpy(reinterpret_cast<uint8_t*>(out) + sizeof(Ngs2RackOption), raw + Ps4CommonSize, tail);
	common->size = sizeof(Ngs2RackOption) + tail;
	return common;
}

// Checks the rack options that sizing and creating a rack both reject.
static int Ngs2CheckRackOption(uint32_t rack_id, const Ngs2RackOption& option) {
	if (rack_id != 0x3000) {
		return OK;
	}
	EXIT_NOT_IMPLEMENTED(option.size != sizeof(Ngs2MasteringRackOption));
	const auto& mastering = reinterpret_cast<const Ngs2MasteringRackOption&>(option);
	if (mastering.max_channels - 1 >= 37) {
		return NGS2_ERROR_INVALID_NUM_CHANNELS;
	}
	return mastering.num_peak_meter_blocks - 1 >= 256 ? NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS
	                                                  : OK;
}

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option,
                                          Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option     = nullptr, using reset defaults for rack_id 0x%" PRIx32 "\n", rack_id);
	}
	Ngs2RackOptionUnion native_option {};
	option = Ngs2NativeRackOption(option, &native_option);

	LOGF("\t rack_id    = 0x%" PRIx32 "\n"
	     "\t max_voices = %u\n",
	     rack_id, option->max_voices);

	const auto result = Ngs2CheckRackOption(rack_id, *option);
	if (result != OK) {
		return result;
	}

	buffer_info->host_buffer_size =
	    sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * option->max_voices;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption*    option,
                                                const Ngs2BufferAllocator* allocator,
                                                uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}
	Ngs2SystemOption native_option {};
	option = Ngs2NativeSystemOption(option, &native_option);

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	LOGF("\t name              = %.64s\n"
	     "\t flags             = %u\n"
	     "\t max_grain_samples = %u\n"
	     "\t num_grain_samples = %u\n"
	     "\t sample_rate       = %u\n"
	     "\t max_voice_channels = %u\n"
	     "\t alloc_handler     = 0x%016" PRIx64 "\n"
	     "\t free_handler      = 0x%016" PRIx64 "\n"
	     "\t user_data         = 0x%016" PRIx64 "\n",
	     option->name, option->flags, option->max_grain_samples, option->num_grain_samples,
	     option->sample_rate, option->max_voice_channels,
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = sizeof(Ngs2Internal);
	buf.user_data        = allocator->user_data;

	int result = Cpu::CallGuest<int32_t>(allocator->alloc_handler, &buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	auto* ngs      = Ngs2CreateSystemInternal(option, &buf);
	ngs->allocator = *allocator;

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size) {
	constexpr int32_t ERROR_INVALID_OUT_ADDRESS   = static_cast<int32_t>(0x804a8010u);
	constexpr int32_t ERROR_INVALID_OUT_SIZE      = static_cast<int32_t>(0x804a8011u);

	if (info == nullptr) {
		return ERROR_INVALID_OUT_ADDRESS;
	}
	if (info_size != sizeof(Ngs2SystemInfo)) {
		return ERROR_INVALID_OUT_SIZE;
	}

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	Common::LockGuard lock(ngs->mutex);

	*info = {};
	std::memcpy(info->name, ngs->option.name, sizeof(info->name));
	info->system_handle     = system_handle;
	info->buffer_info       = ngs->buffer_info;
	info->uid               = ngs->uid;
	info->min_grain_samples = 64;
	info->max_grain_samples = ngs->option.max_grain_samples;
	info->state_flags       = 1;
	info->render_count      = ngs->render_count;
	info->sample_rate       = ngs->option.sample_rate;
	info->num_grain_samples = ngs->option.num_grain_samples;

	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs == ngs) {
			info->rack_count++;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n"
	     "\t num_samples   = %u\n",
	     static_cast<uint64_t>(system_handle), num_samples);

	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	auto*             ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	Common::LockGuard lock(ngs->mutex);
	ngs->option.num_grain_samples = num_samples;

	return OK;
}

// Disconnects the ports of the racks of `ngs` whose destination lies in [first, first + size).
// The caller holds g_racks_mutex.
static void Ngs2DisconnectPorts(const Ngs2Internal* ngs, uintptr_t first, uintptr_t size) {
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs != ngs) {
			continue;
		}
		auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
			for (auto& port: voices[i].ports) {
				if (reinterpret_cast<uintptr_t>(port.dest) - first < size) {
					port.dest = nullptr;
				}
			}
		}
	}
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(system_handle));

	Ngs2Internal* ngs = nullptr;
	{
		Common::LockGuard lock(g_ngs_mutex);
		ngs = Ngs2Unlink(g_ngs_list, system_handle);
	}
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	{
		// Waits for a render in progress. The racks are disconnected from every patch.
		Common::LockGuard lock(ngs->mutex);
		Common::LockGuard racks_lock(g_racks_mutex);
		Ngs2DisconnectPorts(ngs, 0, UINTPTR_MAX);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs == ngs) {
				rack->ngs = &g_detached_system;
			}
		}
	}
	auto       context_buffer = ngs->buffer_info;
	const auto free_handler   = ngs->allocator.free_handler;
	std::destroy_at(ngs);
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}
	// The result of the free handler is ignored.
	if (free_handler != nullptr) {
		Cpu::CallGuest<int32_t>(free_handler, &context_buffer);
	}

	return OK;
}

static void Ngs2InitModules(Ngs2RackInternal& rack) {
	if (!Ngs2RackIsCustom(rack.type)) {
		return;
	}
	const auto& custom = rack.option.custom_sampler.custom_rack_option;
#if defined(__ANDROID__)
	// `debug.capy.ngs2_log 1`: describe each custom rack (tag CapyPS5-Ngs2).
	static Common::DebugFlags::CachedValue ngs2_log("debug.capy.ngs2_log", 0);
	if (ngs2_log.Get() != 0) {
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Ngs2",
		                    "custom rack type=%d buffers=%u modules=%u ports=%u voices=%u",
		                    static_cast<int>(rack.type), custom.num_buffers, custom.num_modules,
		                    rack.option.common.max_ports, rack.option.common.max_voices);
		for (uint32_t m = 0; m < custom.num_modules; ++m) {
			const auto& info = custom.module[m];
			const auto* raw  = reinterpret_cast<const uint32_t*>(info.option);
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Ngs2",
			                    "  module %u id=0x%02x src=%u extra=%u dst=%u state=%u+%u "
			                    "option=%08x %08x %08x %08x %08x %08x",
			                    m, info.module_id, info.source_buffer_id, info.extra_buffer_id,
			                    info.dest_buffer_id, info.state_offset, info.state_size,
			                    raw ? raw[0] : 0, raw ? raw[1] : 0, raw ? raw[2] : 0,
			                    raw ? raw[3] : 0, raw ? raw[4] : 0, raw ? raw[5] : 0);
		}
	}
#endif
	EXIT_NOT_IMPLEMENTED(custom.num_buffers == 0 || custom.num_buffers > 4 ||
	                     rack.option.common.max_ports > 16);
	for (uint32_t p = 0; p < rack.option.common.max_ports; ++p) {
		EXIT_NOT_IMPLEMENTED(custom.port[p].source_buffer_id >= custom.num_buffers);
	}
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(&rack + 1);
	for (uint32_t m = 0; m < custom.num_modules; ++m) {
		const auto& info = custom.module[m];
		// Modules process their source buffer in place, so skipped modules pass audio through.
		EXIT_NOT_IMPLEMENTED(info.source_buffer_id != info.dest_buffer_id ||
		                     std::max(info.source_buffer_id, info.extra_buffer_id) >=
		                         custom.num_buffers);
		if (info.module_id != 0x1f) {
			continue;
		}
		auto& fx = rack.fx[m];
		fx       = *reinterpret_cast<const Ngs2UserFx2Option*>(info.option);
		rack.common[m].resize(fx.common_size);
		for (uint32_t i = 0; i < rack.option.common.max_voices; ++i) {
			auto& module = voices[i].modules[m];
			module.param.resize(fx.param_size);
			module.work.resize(fx.work_size);
			module.state.resize(custom.module[m].state_size);
			if (fx.setup != nullptr) {
				Ngs2UserFx2Context context {
				    rack.common[m].data(), module.param.data(),           module.work.data(),
				    fx.user_data,          rack.option.common.max_voices, i};
				EXIT_NOT_IMPLEMENTED(Cpu::CallGuest<int>(fx.setup, &context) != OK);
			}
		}
	}
}

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id,
                                 const Ngs2RackOption*        option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}
	Ngs2RackOptionUnion native_option {};
	option = Ngs2NativeRackOption(option, &native_option);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));
	const auto result = Ngs2CheckRackOption(rack_id, *option);
	if (result != OK) {
		return result;
	}

	LOGF("\t rack_id                = 0x%" PRIx32 "\n"
	     "\t name                   = %.64s\n"
	     "\t flags                  = %u\n"
	     "\t max_grain_samples      = %u\n"
	     "\t max_voices             = %u\n"
	     "\t max_input_delay_blocks = %u\n"
	     "\t max_matrices           = %u\n"
	     "\t max_ports              = %u\n"
	     "\t max_voice_channels     = %u\n"
	     "\t max_output_channels    = %u\n"
	     "\t host_buffer            = 0x%016" PRIx64 "\n"
	     "\t host_buffer_size      = 0x%016" PRIx64 "\n",
	     rack_id, option->name, option->flags, option->max_grain_samples, option->max_voices,
	     option->max_input_delay_blocks, option->max_matrices, option->max_ports,
	     option->max_voice_channels, option->max_output_channels,
	     reinterpret_cast<uint64_t>(buffer_info->host_buffer),
	     static_cast<uint64_t>(buffer_info->host_buffer_size));

	auto* ngs    = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* rack   = new (buffer_info->host_buffer) Ngs2RackInternal {};
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

	Common::LockGuard lock(ngs->mutex);

	switch (rack_id) {
		case 0x1000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SamplerRackOption));
			rack->option.sampler = *reinterpret_cast<const Ngs2SamplerRackOption*>(option);
			rack->type           = Ngs2RackType::Sampler;
			break;
		case 0x2000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SubmixerRackOption));
			rack->option.submixer = *reinterpret_cast<const Ngs2SubmixerRackOption*>(option);
			rack->type            = Ngs2RackType::Submixer;
			break;
		case 0x2001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2ReverbRackOption));
			rack->option.reverb = *reinterpret_cast<const Ngs2ReverbRackOption*>(option);
			rack->type          = Ngs2RackType::Reverb;
			break;
		case 0x3000:
			rack->option.mastering = *reinterpret_cast<const Ngs2MasteringRackOption*>(option);
			rack->type             = Ngs2RackType::Mastering;
			break;
		case 0x4002:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSubmixerRackOption));
			rack->option.custom_submixer =
			    *reinterpret_cast<const Ngs2CustomSubmixerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSubmixer;
			break;
		case 0x4003:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomMasteringRackOption));
			rack->option.custom_mastering =
			    *reinterpret_cast<const Ngs2CustomMasteringRackOption*>(option);
			rack->type = Ngs2RackType::CustomMastering;
			break;
		case 0x4001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSamplerRackOption));
			rack->option.custom_sampler =
			    *reinterpret_cast<const Ngs2CustomSamplerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSampler;
			break;
		default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
	}

	LOGF("\t type                   = %s\n", magic_enum::enum_name(rack->type));

	rack->allocator   = Ngs2BufferAllocator();
	rack->buffer_info = *buffer_info;
	rack->ngs         = ngs;

	{
		Common::LockGuard racks_lock(g_racks_mutex);
		rack->next   = g_racks_list;
		g_racks_list = rack;
	}

	for (uint32_t i = 0; i < option->max_voices; i++) {
		auto& voice = *std::construct_at(voices + i);
		voice.rack  = rack;
		voice.ports.resize(option->max_ports);
		voice.matrices.resize(option->max_matrices);
		if (rack->type == Ngs2RackType::Mastering) {
			voice.mastering = std::make_unique<Ngs2VoiceInternal::Mastering>();
			voice.mastering->meter_blocks.resize(size_t(2) * rack->option.mastering.max_channels *
			                                     rack->option.mastering.num_peak_meter_blocks);
		}
		if (Ngs2RackIsCustom(rack->type)) {
			voice.modules.resize(rack->option.custom_sampler.custom_rack_option.num_modules);
		}
	}
	Ngs2InitModules(*rack);

	*handle = reinterpret_cast<uintptr_t>(rack);

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id,
                                              const Ngs2RackOption*      option,
                                              const Ngs2BufferAllocator* allocator,
                                              uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}
	Ngs2RackOptionUnion native_option {};
	option = Ngs2NativeRackOption(option, &native_option);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	LOGF("\t alloc_handler          = 0x%016" PRIx64 "\n"
	     "\t free_handler           = 0x%016" PRIx64 "\n"
	     "\t user_data              = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = 0;
	buf.user_data        = allocator->user_data;

	int result = Ngs2RackQueryBufferSize(rack_id, option, &buf);
	if (result != OK) {
		return result;
	}

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	result = Cpu::CallGuest<int32_t>(allocator->alloc_handler, &buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK) {
		auto* rack      = static_cast<Ngs2RackInternal*>(buf.host_buffer);
		rack->allocator = *allocator;
	}

	return result;
}

// The created rack with this handle, or nullptr.
static Ngs2RackInternal* Ngs2FindRack(uintptr_t handle) {
	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (reinterpret_cast<uintptr_t>(rack) == handle) {
			return rack;
		}
	}
	return nullptr;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	Ngs2RackInternal* rack = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		rack = Ngs2Unlink(g_racks_list, rack_handle);
	}
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	{
		// Waits for a render in progress, which collected the rack before it was unlinked, and
		// disconnects the ports whose destination is a voice of the rack.
		Common::LockGuard lock(rack->ngs->mutex);
		Common::LockGuard racks_lock(g_racks_mutex);
		Ngs2DisconnectPorts(rack->ngs, reinterpret_cast<uintptr_t>(voices),
		                    sizeof(Ngs2VoiceInternal) * rack->option.common.max_voices);
	}
	for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
		for (size_t m = 0; m < voices[i].modules.size(); ++m) {
			const auto& fx = rack->fx[m];
			if (fx.cleanup != nullptr) {
				auto&              module = voices[i].modules[m];
				Ngs2UserFx2Context context {rack->common[m].data(),
				                            module.param.data(),
				                            module.work.data(),
				                            fx.user_data,
				                            rack->option.common.max_voices,
				                            i};
				EXIT_NOT_IMPLEMENTED(Cpu::CallGuest<int>(fx.cleanup, &context) != OK);
			}
		}
		std::destroy_at(voices + i);
	}
	auto       context_buffer = rack->buffer_info;
	const auto free_handler   = rack->allocator.free_handler;
	std::destroy_at(rack);
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}
	// The result of the free handler is ignored.
	if (free_handler != nullptr) {
		Cpu::CallGuest<int32_t>(free_handler, &context_buffer);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	rack->user_lock.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	rack->user_lock.Unlock();

	return OK;
}

struct Ngs2VoiceCallbackInfo {
	uintptr_t   data, voice;
	uint32_t    flag, reserved;
	uintptr_t   user;
	const void* block_data;
	uint32_t    size, repeats, attributes, reserved2;
};
static_assert(sizeof(Ngs2VoiceCallbackInfo) == 56);
static_assert(offsetof(Ngs2VoiceCallbackInfo, size) == 40 &&
              offsetof(Ngs2VoiceCallbackInfo, repeats) == 44 &&
              offsetof(Ngs2VoiceCallbackInfo, attributes) == 48);

static bool Ngs2FinishBlock(Ngs2VoiceInternal& voice) {
	const auto generation = voice.source_generation;
	auto&      block      = voice.blocks.front();
	const bool repeat     = block.info.num_repeats != 0;
	voice.waveform_end = voice.WaveformData();
	if (repeat) {
		if (block.info.num_repeats != UINT32_MAX) {
			--block.info.num_repeats;
		}
		++block.repeated_count;
		block.data_cursor = 0;
		if (voice.decoder != nullptr) {
			voice.compressed_input.clear();
			voice.decoder->Reset();
		}
	}
	const Ngs2VoiceCallbackInfo info {voice.callback_data,
	                                  reinterpret_cast<uintptr_t>(&voice),
	                                  repeat ? 2u : 1u,
	                                  0,
	                                  block.info.user_data,
	                                  block.data,
	                                  static_cast<uint32_t>(block.info.data_size),
	                                  block.repeated_count,
	                                  0,
	                                  0};
	if (!repeat) {
		voice.blocks.pop_front();
	}
	if (voice.callback != 0 && (voice.callback_flags & info.flag) != 0) {
		Cpu::CpuBackend::Instance()->CallGuestCallback(
		    voice.callback, reinterpret_cast<uint64_t>(&info));
	}
	return voice.source_generation == generation;
}

static bool Ngs2DecodeFrame(Ngs2VoiceInternal& voice, std::vector<float>& frame,
                            uint32_t& frame_samples) {
	frame_samples = 0;
	while (voice.state == Ngs2VoicePlayState::Playing && !voice.blocks.empty()) {
		auto& block = voice.blocks.front();
		if (!Ngs2IsDataOnly(block.info) && block.data_cursor == 0) {
			// PCM plays the skip prefix; the decoder drops it.
			voice.duration_remaining = block.info.num_samples +
			                           (voice.decoder == nullptr ? block.info.num_skip_samples : 0);
			voice.skip_remaining     = block.info.num_skip_samples;
		}
		if (voice.duration_remaining == 0) {
			// A block outside any samples' extent is dropped, whatever its repeats.
			block.info.num_repeats = 0;
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
			continue;
		}
		if (voice.decoder == nullptr) {
			// PCM has no encoded frame size, so stage at most one system grain.
			const auto quantum     = std::max(voice.rack->ngs->option.num_grain_samples, 1u);
			const auto frame_bytes = voice.channels * sizeof(int16_t);
			const auto available   = (block.info.data_size - block.data_cursor) / frame_bytes;
			const auto count = std::min<size_t>({available, voice.duration_remaining, quantum});
			frame.resize(size_t(quantum) * voice.channels);
			const auto* pcm = reinterpret_cast<const int16_t*>(block.data + block.data_cursor);
			for (size_t i = 0; i < count * voice.channels; ++i) {
				frame[i] = pcm[i] / 32768.0f;
			}
			// The skip prefix does not count as decoded.
			const auto skipped = std::min<size_t>(voice.skip_remaining, count);
			voice.skip_remaining -= skipped;
			voice.duration_remaining -= count;
			block.data_cursor += count * frame_bytes;
			voice.decoded_bytes += count * frame_bytes;
			voice.decoded_samples += count - skipped;
			frame_samples = static_cast<uint32_t>(count);
			if (voice.duration_remaining == 0 ||
			    block.info.data_size - block.data_cursor < frame_bytes) {
				// A repeating block restarts its duration, so it must hold all of it.
				EXIT_NOT_IMPLEMENTED(voice.duration_remaining != 0 &&
				                     (block.data_cursor != block.info.data_size ||
				                      block.info.num_repeats != 0));
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
			}
			if (count != 0) {
				return true;
			}
			continue;
		}
		const auto skip =
		    std::min(voice.skip_remaining,
		             static_cast<uint32_t>(frame.size() / voice.channels));
		Ajm::AjmGaplessState gapless;
		gapless.Set({voice.duration_remaining, static_cast<uint16_t>(skip), 0}, true);
		if (!voice.compressed_input.empty()) {
			Ajm::AjmSidebandDecAt9CodecInfo info {};
			voice.decoder->WriteCodecInfo(&info, sizeof(info), {});
			EXIT_NOT_IMPLEMENTED(info.next_frame_size < voice.compressed_input.size());
			const auto bytes =
			    std::min<size_t>(info.next_frame_size - voice.compressed_input.size(),
			                     block.info.data_size - block.data_cursor);
			voice.compressed_input.insert(voice.compressed_input.end(),
			                              block.data + block.data_cursor,
			                              block.data + block.data_cursor + bytes);
			block.data_cursor += bytes;
			voice.decoded_bytes += bytes;
			if (voice.compressed_input.size() < info.next_frame_size) {
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
				continue;
			}
		}
		const bool pending = !voice.compressed_input.empty();
		const auto result  = voice.decoder->Decode(
		    pending ? voice.compressed_input.data() : block.data + block.data_cursor,
		    pending ? voice.compressed_input.size() : block.info.data_size - block.data_cursor,
		    frame.data(), frame.size() * sizeof(float), false,
		    &gapless);
		if (!pending && result.result == Ajm::AJM_RESULT_PARTIAL_INPUT &&
		    result.input_consumed == 0) {
			voice.compressed_input.insert(voice.compressed_input.end(),
			                              block.data + block.data_cursor,
			                              block.data + block.info.data_size);
			voice.decoded_bytes += block.info.data_size - block.data_cursor;
			block.data_cursor = block.info.data_size;
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result.result != OK || result.input_consumed == 0);
		if (pending) {
			voice.compressed_input.erase(voice.compressed_input.begin(),
			                             voice.compressed_input.begin() + result.input_consumed);
		} else {
			block.data_cursor += result.input_consumed;
			voice.decoded_bytes += result.input_consumed;
		}
		voice.skip_remaining -= skip - gapless.current.skip_samples;
		const auto decoded_samples =
		    static_cast<uint32_t>(result.output_written / (sizeof(float) * voice.channels));
		if (decoded_samples != 0) {
			EXIT_NOT_IMPLEMENTED(decoded_samples > voice.duration_remaining);
			frame_samples = decoded_samples;
			voice.duration_remaining -= decoded_samples;
			voice.decoded_samples += decoded_samples;
		}
		if (voice.duration_remaining == 0 ||
		    (block.data_cursor == block.info.data_size && voice.compressed_input.empty())) {
			if (voice.duration_remaining == 0) {
				voice.compressed_input.clear();
			}
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
		}
		if (decoded_samples != 0) {
			return true;
		}
	}
	return false;
}

static bool Ngs2CurrentFrame(Ngs2VoiceInternal& voice) {
	if (voice.frame_cursor == voice.frame_samples) {
		voice.frame_cursor  = 0;
		voice.frame_samples = 0;
		if (voice.next_frame_samples != 0) {
			std::swap(voice.decoded_frame, voice.next_decoded_frame);
			voice.frame_samples      = voice.next_frame_samples;
			voice.next_frame_samples = 0;
		} else if (!Ngs2DecodeFrame(voice, voice.decoded_frame, voice.frame_samples)) {
			return false;
		}
	}
	return voice.state == Ngs2VoicePlayState::Playing &&
	       voice.frame_cursor < voice.frame_samples;
}

static void Ngs2AdvanceSamples(Ngs2VoiceInternal& voice) {
	const auto output_rate = uint64_t(voice.rack->ngs->option.sample_rate) << 32u;
	while (voice.state == Ngs2VoicePlayState::Playing && voice.sample_phase >= output_rate) {
		if (!Ngs2CurrentFrame(voice)) {
			break;
		}
		const auto advance = std::min(voice.sample_phase / output_rate,
		                              uint64_t(voice.frame_samples - voice.frame_cursor));
		voice.frame_cursor += static_cast<uint32_t>(advance);
		voice.sample_phase -= advance * output_rate;
	}
	if (voice.blocks.empty() && voice.frame_cursor == voice.frame_samples &&
	    voice.next_frame_samples == 0 && !voice.accepts_blocks) {
		voice.sample_phase = 0;
	}
}

static void Ngs2ConsumeSamples(Ngs2VoiceInternal& voice, uint32_t grain) {
	const auto generation = voice.source_generation;
	uint32_t   output     = 0;
	const auto output_rate = uint64_t(voice.rack->ngs->option.sample_rate) << 32u;
	while (voice.state == Ngs2VoicePlayState::Playing && output < grain &&
	       (!voice.blocks.empty() || voice.frame_cursor < voice.frame_samples ||
	        voice.next_frame_samples != 0)) {
		Ngs2AdvanceSamples(voice);
		if (voice.source_generation != generation || !Ngs2CurrentFrame(voice)) {
			break;
		}
		if (voice.frame_cursor + 1 == voice.frame_samples && voice.sample_phase != 0) {
			if (voice.next_frame_samples == 0 && !voice.blocks.empty()) {
				if (voice.next_decoded_frame.empty()) {
					voice.next_decoded_frame.resize(voice.decoded_frame.size());
				}
				Ngs2DecodeFrame(voice, voice.next_decoded_frame, voice.next_frame_samples);
			}
			if (voice.source_generation != generation ||
			    voice.state != Ngs2VoicePlayState::Playing ||
			    voice.frame_cursor >= voice.frame_samples) {
				break;
			}
		}
		const float* current = voice.decoded_frame.data() + voice.frame_cursor * voice.channels;
		const float* next = voice.frame_cursor + 1 < voice.frame_samples
		                        ? current + voice.channels
		                        : voice.next_frame_samples != 0 ? voice.next_decoded_frame.data()
		                                                        : current;
		const float fraction = static_cast<float>(voice.sample_phase) /
		                       static_cast<float>(output_rate);
		for (uint32_t c = 0; c < voice.channels; ++c) {
			voice.samples[c * grain + output] = std::lerp(current[c], next[c], fraction);
		}
		voice.sample_phase += voice.sample_step;
		++output;
	}
	if (voice.source_generation == generation) {
		Ngs2AdvanceSamples(voice);
	}
	if (voice.source_generation != generation) {
		// A callback replaced the source, possibly changing its channel layout.
		voice.ClearSamples(grain);
		output = 0;
	}
	voice.has_samples = output != 0;
}

// Mono and stereo convert at -3 dB, mono feeds the surround front center, others map 1:1.
static float Ngs2DefaultMatrixLevel(uint32_t src_channels, uint32_t src, uint32_t dst_channels,
                                    size_t dst) {
	if ((src_channels == 1 && dst_channels == 2) || (src_channels == 2 && dst_channels == 1)) {
		return std::numbers::sqrt2_v<float> / 2;
	}
	if (src_channels == 1 && dst_channels > 2) {
		return dst == 2 ? 1.0f : 0.0f;
	}
	return src == dst ? 1.0f : 0.0f;
}

static void Ngs2RenderVoice(Ngs2VoiceInternal& voice, const std::vector<Ngs2VoiceInternal*>& voices,
                            uint32_t grain) {
	if (voice.rendered) {
		return;
	}
	if (voice.channels == 0) {
		voice.has_samples = false;
		voice.rendered    = true;
		return;
	}
	EXIT_NOT_IMPLEMENTED(voice.rendering);
	voice.rendering = true;
	voice.ClearSamples(grain);
	voice.has_samples = false;
	if (voice.state == Ngs2VoicePlayState::Playing) {
		if (voice.rack->type == Ngs2RackType::Sampler ||
		    voice.rack->type == Ngs2RackType::CustomSampler) {
			Ngs2ConsumeSamples(voice, grain);
			// A playing stream can have no source data but still have a filter tail, at any
			// location. The front filters run before the rear ones, each in index order.
			if (voice.has_samples ||
			    std::ranges::any_of(voice.filters, &Ngs2SamplerFilter::HasTail)) {
				voice.RunFilters(0, voice.samples.data(), grain);
				voice.RunFilters(1, voice.samples.data(), grain);
				voice.has_samples = true;
			}
			// The voice ends in the grain its data runs out; the filter tails end with it.
			if (voice.state == Ngs2VoicePlayState::Playing && voice.blocks.empty() &&
			    voice.frame_cursor == voice.frame_samples && voice.next_frame_samples == 0 &&
			    !voice.accepts_blocks) {
				voice.state = Ngs2VoicePlayState::Empty;
			}
		}
		for (auto* source: voices) {
			for (size_t p = 0; p < source->ports.size(); ++p) {
				const auto& port = source->ports[p];
				if (port.dest != &voice) {
					continue;
				}
				Ngs2RenderVoice(*source, voices, grain);
				if (!source->has_samples) {
					continue;
				}
				const auto* input = source->Buffer(source->rack->PortBufferId(p), grain);
				// The pre-send filters of port N (location 2 + N) run on a copy of its send, also
				// when the port is muted.
				const auto location = static_cast<uint32_t>(p + 2);
				if (std::ranges::find(source->filters, location, &Ngs2SamplerFilter::location) !=
				    source->filters.end()) {
					source->mix_samples.assign(input, input + size_t(source->channels) * grain);
					source->RunFilters(location, source->mix_samples.data(), grain);
					input = source->mix_samples.data();
				}
				if (port.volume == 0.0f) {
					continue;
				}
				// Input N of a custom rack voice feeds its buffer N.
				EXIT_NOT_IMPLEMENTED(port.input >= voice.rack->NumBuffers());
				auto* output = voice.Buffer(port.input, grain);
				// A matrix the game never filled mixes like no matrix (Animal Well sends every
				// sampler through matrix 0 without setting it, which mixed nothing).
				const auto* matrix = port.matrix < 0 || source->matrices.at(port.matrix).empty()
				                         ? nullptr
				                         : &source->matrices.at(port.matrix);
				const size_t output_channels =
				    matrix == nullptr
				        ? voice.channels
				        : std::min<size_t>(voice.channels, matrix->size() / source->channels);
				for (size_t dst = 0; dst < output_channels; ++dst) {
					for (uint32_t src = 0; src < source->channels; ++src) {
						const float level =
						    port.volume * (matrix == nullptr
						                       ? Ngs2DefaultMatrixLevel(source->channels, src,
						                                                voice.channels, dst)
						                       : (*matrix)[dst * source->channels + src]);
						for (uint32_t i = 0; i < grain; ++i) {
							output[dst * grain + i] += input[src * grain + i] * level;
						}
					}
				}
				voice.has_samples = true;
			}
		}
		if (voice.has_samples) {
			const auto& custom = voice.rack->option.custom_sampler.custom_rack_option;
			const auto  rate   = voice.rack->ngs->option.sample_rate;
			for (size_t m = 0; m < voice.modules.size(); ++m) {
				const auto& info   = custom.module[m];
				auto&       module = voice.modules[m];
				if (info.module_id == 0x17) {
					voice.MixChannels(module.channel_matrix, info.source_buffer_id, grain);
					continue;
				}
				if (info.module_id != 0x19 && info.module_id != 0x1f) {
					if (!module.render_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom module 0x%02x at index %zu is ignored during render\n",
						    info.module_id, m));
						module.render_warned = true;
					}
					continue;
				}
				if (info.module_id == 0x19 && module.handler == nullptr) {
					continue;
				}
				// Handlers receive the source channels followed by the extra buffer's channels.
				std::vector<float*> channels(2 * size_t(voice.channels));
				auto*               source = voice.Buffer(info.source_buffer_id, grain);
				auto*               extra  = voice.Buffer(info.extra_buffer_id, grain);
				for (uint32_t c = 0; c < voice.channels; ++c) {
					channels[c]                  = source + c * grain;
					channels[voice.channels + c] = extra + c * grain;
				}
				if (info.module_id == 0x19) {
					Ngs2UserFxProcessContext context {channels.data(), module.user_data,
					                                  module.flags,    voice.channels,
					                                  grain,           rate};
					module.handler(&context);
				} else {
					const auto&               fx = voice.rack->fx[m];
					Ngs2UserFx2ProcessContext context {channels.data(),
					                                   voice.rack->common[m].data(),
					                                   module.param.data(),
					                                   module.work.data(),
					                                   module.state.data(),
					                                   fx.user_data,
					                                   module.flags,
					                                   voice.channels,
					                                   voice.channels,
					                                   grain,
					                                   rate};
					EXIT_NOT_IMPLEMENTED(Cpu::CallGuest<int>(fx.process, &context) != OK);
				}
				module.flags = 0;
			}
		}
	}
	voice.rendering = false;
	voice.rendered  = true;
}

// A sample clamped to -1..1, scaled by 32767 and truncated; NaN gives 0.
static int16_t Ngs2ToInt16(float sample) {
	return std::isnan(sample) ? int16_t {0}
	                          : static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
}

// Writes the voice's first channels, as many as both have, into render buffer output_id. An int16
// buffer takes this voice's grain alone, zero in its other channels; a float buffer adds it. An
// output_id past the render buffers writes nothing.
static void Ngs2MixOutput(const Ngs2VoiceInternal& voice, const Ngs2RenderBufferInfo* buffer_info,
                          uint32_t num_buffer_info, uint32_t grain) {
	if (voice.output_id >= num_buffer_info) {
		return;
	}
	const auto& output = buffer_info[voice.output_id];
	EXIT_NOT_IMPLEMENTED(output.buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(output.waveform_type != 0x12 && output.waveform_type != 0x18);
	EXIT_NOT_IMPLEMENTED(output.buffer_size < grain * output.num_channels *
	                                               (output.waveform_type == 0x12 ? sizeof(int16_t)
	                                                                             : sizeof(float)));
	const uint32_t frame    = output.num_channels;
	const uint32_t channels = std::min(voice.channels, frame);
	if (output.waveform_type == 0x12) {
		auto* pcm = static_cast<int16_t*>(output.buffer);
		std::fill_n(pcm, size_t(grain) * frame, int16_t {0});
		for (uint32_t c = 0; c < channels; ++c) {
			for (uint32_t i = 0; i < grain; ++i) {
				pcm[i * frame + c] = Ngs2ToInt16(voice.samples[c * grain + i]);
			}
		}
		return;
	}
	auto* pcm = static_cast<float*>(output.buffer);
	for (uint32_t c = 0; c < channels; ++c) {
		for (uint32_t i = 0; i < grain; ++i) {
			pcm[i * frame + c] += voice.samples[c * grain + i];
		}
	}
}

// The rate the LFE filter is designed at for a system rate: the next listed rate, the 88200 design
// up to 96000, and the 11025 design above 192000.
static uint32_t Ngs2MasteringLfeDesignRate(uint32_t rate) {
	constexpr std::array<uint32_t, 9> limits = {11025, 12000, 22050,  24000, 44100,
	                                            48000, 96000, 176400, 192000};
	const auto                        limit  = std::ranges::lower_bound(limits, rate);
	if (limit == limits.end()) {
		return 11025;
	}
	return *limit == 96000 ? 88200 : *limit;
}

// Sets the LFE sections to the 4th-order 0.1 dB Chebyshev low pass at `cutoff` Hz, designed at the
// system rate's design rate.
static void Ngs2SetMasteringLfe(Ngs2VoiceInternal::Mastering& mastering, uint32_t cutoff,
                                uint32_t system_rate) {
	constexpr double ripple_db = 0.1;
	const double     epsilon   = std::sqrt(std::pow(10.0, ripple_db / 10.0) - 1.0);
	const double     mu        = std::asinh(1.0 / epsilon) / 4.0;
	const double     warped =
	    std::tan(std::numbers::pi * cutoff / Ngs2MasteringLfeDesignRate(system_rate));
	double gain = std::pow(10.0, -ripple_db / 20.0);
	for (size_t i = 0; i < mastering.lfe.size(); ++i) {
		// The bilinear transform of the prewarped analog pole re + j im.
		const double angle  = (i == 0 ? 3.0 : 1.0) * std::numbers::pi / 8.0;
		const double re     = -warped * std::sinh(mu) * std::sin(angle);
		const double im     = warped * std::cosh(mu) * std::cos(angle);
		const double d      = (1.0 - re) * (1.0 - re) + im * im;
		mastering.lfe[i].a1 = static_cast<float>(2.0 * (1.0 - re * re - im * im) / d);
		mastering.lfe[i].a2 = static_cast<float>(-((1.0 + re) * (1.0 + re) + im * im) / d);
		gain *= (re * re + im * im) / d;
	}
	// The first section carries the gain of both.
	mastering.lfe[0].gain = static_cast<float>(gain);
	mastering.lfe[1].gain = 1.0f;
}

// The limiter coefficient of a time constant of `time` seconds, counted in whole samples.
static float Ngs2LimiterCoefficient(uint32_t rate, float time) {
	const auto samples = static_cast<uint32_t>(static_cast<float>(rate) * time);
	return samples == 0 ? 0.0f : std::exp(-1.0f / static_cast<float>(samples));
}

// Limits `channels` channels of `grain` samples in place, in blocks of 32 samples.
static void Ngs2Limit(Ngs2VoiceInternal::Mastering& mastering, float* samples, uint32_t channels,
                      uint32_t grain) {
	auto& g = mastering.limiter_gain;
	for (uint32_t start = 0; start < grain; start += 32) {
		const uint32_t length = std::min(grain - start, 32u);
		float          peak   = 0.0f;
		for (uint32_t c = 0; c < channels; ++c) {
			const auto* data = samples + size_t(c) * grain + start;
			for (uint32_t i = 0; i < length; ++i) {
				peak = std::max(peak, std::abs(data[i]));
			}
		}
		const float threshold = mastering.limiter_threshold;
		const float target    = peak > threshold ? threshold / peak : 1.0f;
		const float a         = g > target ? mastering.limiter_attack : mastering.limiter_release;
		// g = a * g + (1 - a) * target per sample, four samples at a time: the first four from the
		// block's start gain, every next four from the previous four. Samples past the last whole
		// four are left as they are.
		const float          a2   = a * a;
		const float          a3   = a2 * a;
		const float          a4   = a2 * a2;
		const float          b    = (1.0f - a) * target;
		const float          c4   = b * (1.0f + a + a2 + a3);
		std::array<float, 4> step = {g * a + b, g * a2 + b * (1.0f + a),
		                             g * a3 + b * (1.0f + a + a2), g * a4 + c4};

		std::array<float, 32> gains;
		const uint32_t        limited = length / 4 * 4;
		for (uint32_t i = 0; i < limited; i += 4) {
			std::copy(step.begin(), step.end(), gains.begin() + i);
			for (auto& gain: step) {
				gain = std::fma(gain, a4, c4);
			}
		}
		for (uint32_t c = 0; c < channels; ++c) {
			auto* data = samples + size_t(c) * grain + start;
			for (uint32_t i = 0; i < limited; ++i) {
				data[i] *= gains[i];
			}
		}
		g = limited == 0 ? step[3] : gains[limited - 1];
		// The gain takes the target once a step no longer moves it.
		if (std::abs(target - g) < 0.01f && target + a * (g - target) == g) {
			g = target;
		}
		mastering.limiter_state = {peak, g};
	}
}

// Writes this grain's level of each channel into the window of peak meter `meter`, and reports
// the window's peak or RMS per channel.
static void Ngs2MeterPeaks(Ngs2VoiceInternal::Mastering& mastering, size_t meter,
                           const float* samples, uint32_t channels, uint32_t grain,
                           uint32_t blocks) {
	const bool peak = mastering.meter_mode == 1;
	for (uint32_t c = 0; c < channels; ++c) {
		const auto* data  = samples + size_t(c) * grain;
		float       level = 0.0f;
		for (uint32_t i = 0; i < grain; ++i) {
			level = peak ? std::max(level, std::abs(data[i])) : level + data[i] * data[i];
		}
		auto* window = mastering.meter_blocks.data() + (size_t(c) * 2 + meter) * blocks;
		window[mastering.meter_index] = level;
		auto* end                     = window + mastering.meter_count;
		mastering.meter_peaks[meter][c] =
		    peak ? *std::max_element(window, end)
			     : std::sqrt(std::accumulate(window, end, 0.0f) /
			                 static_cast<float>(mastering.meter_count * grain));
	}
}

// Runs a playing mastering voice's input peak meter, channel converter, LFE filter, limiter, gain,
// output peak meter and output in place on its N input channels.
static void Ngs2RenderMastering(Ngs2VoiceInternal& voice, const Ngs2RenderBufferInfo* buffer_info,
                                uint32_t num_buffer_info, uint32_t grain) {
	auto&          mastering = *voice.mastering;
	const uint32_t inputs    = voice.channels;
	const uint32_t blocks    = voice.rack->option.mastering.num_peak_meter_blocks;
	const uint32_t mode      = mastering.meters_enabled ? mastering.meter_detect_type : 0;
	if (mastering.meter_mode != mode) {
		mastering.meter_mode  = mode;
		mastering.meter_index = 0;
		mastering.meter_count = 1;
	}
	const bool metering = mastering.meter_mode != 0;
	if (metering) {
		Ngs2MeterPeaks(mastering, 0, voice.samples.data(), inputs, grain, blocks);
	}
	// The converter writes the output buffer's channels, at most N, and zeroes the rest.
	const uint32_t outputs = std::min(inputs, voice.output_id < num_buffer_info
	                                              ? buffer_info[voice.output_id].num_channels
	                                              : inputs);
	const float*   levels  = mastering.levels.data() + Ngs2MasteringLevelsOffset(inputs, outputs);
	// Levels ramp from the previous grain's unless the shape changed or the voice was played.
	const float* start =
	    mastering.previous_inputs == inputs && mastering.previous_outputs == outputs
	        ? mastering.previous_levels.data()
	        : levels;
	// Every output reads all inputs, so the converter mixes from the inputs swapped into
	// mix_samples.
	std::swap(voice.samples, voice.mix_samples);
	voice.samples.assign(voice.mix_samples.size(), 0.0f);
	for (uint32_t d = 0; d < outputs; ++d) {
		auto* out = voice.samples.data() + size_t(d) * grain;
		for (uint32_t s = 0; s < inputs; ++s) {
			const float from = start[d * inputs + s];
			const float to   = levels[d * inputs + s];
			// A level that is not positive at either end contributes nothing.
			if (!(from > 0.0f || to > 0.0f)) {
				continue;
			}
			const auto* in   = voice.mix_samples.data() + size_t(s) * grain;
			const float step = (to - from) / static_cast<float>(grain);
			for (uint32_t i = 0; i < grain; ++i) {
				out[i] += in[i] * (from + static_cast<float>(i) * step);
			}
		}
	}
	std::copy_n(levels, size_t(inputs) * outputs, mastering.previous_levels.begin());
	mastering.previous_inputs  = inputs;
	mastering.previous_outputs = outputs;

	// The LFE filter runs in place on channel 3, which voices of fewer than 4 channels lack; a
	// grain without the filter zeroes its history.
	if (!mastering.lfe_enabled) {
		mastering.lfe[0].history = mastering.lfe[1].history = {};
	} else if (mastering.lfe_enable_flag && inputs >= 4) {
		auto* samples = voice.samples.data() + size_t(3) * grain;
		for (auto& section: mastering.lfe) {
			auto [gain, a1, a2, h] = section;
			for (uint32_t i = 0; i < grain; ++i) {
				const float x = samples[i];
				const float y = gain * x + 2.0f * gain * h.x1 + gain * h.x2 + a1 * h.y1 + a2 * h.y2;
				h             = {x, h.x1, y, h.y1};
				samples[i]    = y;
			}
			section.history = h;
		}
	}

	// CapyPS5: `debug.capy.ngs2_limiter 0` leaves the output unlimited, as before the limiter.
	static Common::DebugFlags::CachedValue limiter("debug.capy.ngs2_limiter", 1);
	if (mastering.limiter_enabled && mastering.limiter_enable_flag && limiter.Get() != 0) {
		Ngs2Limit(mastering, voice.samples.data(), inputs, grain);
	} else {
		mastering.limiter_gain = 1.0f;
		if (mastering.limiter_enabled) {
			mastering.limiter_state = {0.0f, 1.0f};
		}
	}

	// Channel 3 is LFE, every other channel full band; a changed gain ramps over one grain.
	if (mastering.gain_enabled) {
		const auto& from = mastering.gain_rendered ? mastering.previous_gain : mastering.gain;
		for (uint32_t c = 0; c < inputs; ++c) {
			auto*        data = voice.samples.data() + size_t(c) * grain;
			const size_t k    = c == 3 ? 1 : 0;
			const float  step = (mastering.gain[k] - from[k]) / static_cast<float>(grain);
			for (uint32_t i = 0; i < grain; ++i) {
				data[i] *= from[k] + static_cast<float>(i) * step;
			}
		}
		mastering.previous_gain = mastering.gain;
	}
	mastering.gain_rendered = mastering.gain_enabled;

	if (metering) {
		Ngs2MeterPeaks(mastering, 1, voice.samples.data(), inputs, grain, blocks);
		mastering.meter_index = (mastering.meter_index + 1) % blocks;
		mastering.meter_count = std::min(mastering.meter_count + 1, blocks);
	}
	mastering.meter_channels = metering ? inputs : 0;

	Ngs2MixOutput(voice, buffer_info, num_buffer_info, grain);
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info) {
	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr || system_handle == 0 || num_buffer_info == 0);
	auto* ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	Common::LockGuard lock(ngs->mutex);
	std::vector<Ngs2VoiceInternal*> voices;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs != ngs) {
				continue;
			}
			auto* items = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
			for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
				items[i].rendered = false;
				voices.push_back(items + i);
			}
		}
	}
	// A buffer no voice writes stays silent, and float voices add onto silence.
	for (uint32_t i = 0; i < num_buffer_info; ++i) {
		if (buffer_info[i].buffer != nullptr && buffer_info[i].buffer_size != 0) {
			std::memset(buffer_info[i].buffer, 0, buffer_info[i].buffer_size);
		}
	}
	const auto grain = ngs->option.num_grain_samples;
	for (auto* voice: voices) {
		Ngs2RenderVoice(*voice, voices, grain);
		// A playing mastering voice renders its modules even without input.
		if (voice->rack->type == Ngs2RackType::Mastering &&
		    voice->state == Ngs2VoicePlayState::Playing && voice->channels != 0) {
			Ngs2RenderMastering(*voice, buffer_info, num_buffer_info, grain);
		} else if (voice->rack->type == Ngs2RackType::CustomMastering && voice->has_samples) {
			Ngs2MixOutput(*voice, buffer_info, num_buffer_info, grain);
		}
	}
#if defined(__ANDROID__)
	// `debug.capy.ngs2_log 1`: every 200 renders, log the voices per rack type (total/with
	// samples/playing) and the peak of each output buffer (tag CapyPS5-Ngs2).
	static Common::DebugFlags::CachedValue ngs2_log("debug.capy.ngs2_log", 0);
	if (ngs2_log.Get() != 0 && ngs->render_count % 200 == 0) {
		uint32_t counts[7][3] = {};
		for (const auto* voice: voices) {
			auto& c = counts[static_cast<int>(voice->rack->type)];
			c[0]++;
			c[1] += voice->has_samples ? 1 : 0;
			c[2] += voice->state == Ngs2VoicePlayState::Playing ? 1 : 0;
		}
		char line[512];
		int  used = std::snprintf(line, sizeof(line), "render %llu:", static_cast<unsigned long long>(ngs->render_count));
		for (int t = 0; t < 7; t++) {
			if (counts[t][0] != 0) {
				used += std::snprintf(line + used, sizeof(line) - used, " type%d=%u/%u/%u", t, counts[t][0],
				                      counts[t][1], counts[t][2]);
			}
		}
		for (uint32_t i = 0; i < num_buffer_info && used < 480; ++i) {
			float peak = 0.0f;
			if (buffer_info[i].buffer != nullptr && buffer_info[i].waveform_type == 0x12) {
				const auto* s = static_cast<const int16_t*>(buffer_info[i].buffer);
				for (size_t k = 0; k < buffer_info[i].buffer_size / sizeof(int16_t); k++) {
					peak = std::max(peak, std::fabs(static_cast<float>(s[k]) / 32767.0f));
				}
			} else if (buffer_info[i].buffer != nullptr && buffer_info[i].waveform_type == 0x18) {
				const auto* f = static_cast<const float*>(buffer_info[i].buffer);
				for (size_t k = 0; k < buffer_info[i].buffer_size / sizeof(float); k++) peak = std::max(peak, std::fabs(f[k]));
			}
			used += std::snprintf(line + used, sizeof(line) - used, " out%u(type 0x%x ch %u)=%.4f", i,
			                      buffer_info[i].waveform_type, buffer_info[i].num_channels, peak);
		}
		__android_log_write(ANDROID_LOG_WARN, "CapyPS5-Ngs2", line);
		// Routing of the voices that are not silent samplers: where each port sends, how loud.
		int listed = 0;
		for (const auto* voice: voices) {
			const bool sampler = voice->rack->type == Ngs2RackType::Sampler;
			if ((sampler && !voice->has_samples && voice->state != Ngs2VoicePlayState::Playing) || listed >= 24) {
				continue;
			}
			listed++;
			used = std::snprintf(line, sizeof(line), "  voice %p type%d ch%u state%u samples%d:",
			                     static_cast<const void*>(voice), static_cast<int>(voice->rack->type),
			                     voice->channels, static_cast<unsigned>(voice->state), voice->has_samples ? 1 : 0);
			if (sampler) {
				used += std::snprintf(line + used, sizeof(line) - used, " rate%u blocks%zu at9%d frame%u/%u",
				                      voice->sample_rate, voice->blocks.size(), voice->decoder != nullptr ? 1 : 0,
				                      voice->frame_cursor, voice->frame_samples);
			}
			{
				float peak = 0.0f;
				for (float v: voice->samples) peak = std::max(peak, std::fabs(v));
				used += std::snprintf(line + used, sizeof(line) - used, " peak%.4f", peak);
			}
			if (voice->rack->type == Ngs2RackType::Mastering) {
				const auto& mastering = *voice->mastering;
				used += std::snprintf(line + used, sizeof(line) - used, " gain%d %.3f/%.3f out%u",
				                      mastering.gain_enabled ? 1 : 0, mastering.gain[0], mastering.gain[1],
				                      voice->output_id);
			}
			for (size_t p = 0; p < voice->ports.size() && used < 440; p++) {
				const auto& port = voice->ports[p];
				if (port.dest == nullptr) {
					continue;
				}
				used += std::snprintf(line + used, sizeof(line) - used, " p%zu->%p(type%d) in%u vol%.2f m%d", p,
				                      static_cast<const void*>(port.dest), static_cast<int>(port.dest->rack->type),
				                      port.input, port.volume, port.matrix);
			}
			__android_log_write(ANDROID_LOG_WARN, "CapyPS5-Ngs2", line);
		}
	}
#endif
	for (auto* voice: voices) {
		// No envelope is configured, so a release stop finishes in this render.
		if (voice->state == Ngs2VoicePlayState::Stopped) {
			voice->state = Ngs2VoicePlayState::Empty;
		}
		voice->state_flags = static_cast<uint32_t>(voice->state);
	}
	++ngs->render_count;
	return OK;
}

static uint16_t Ngs2ReadLe16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
}

static uint32_t Ngs2ReadLe32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

static bool Ngs2FourCcEquals(const uint8_t* data, const char* four_cc) {
	return std::memcmp(data, four_cc, 4) == 0;
}

static bool Ngs2GetAtrac9CodecInfo(const std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE>& config,
                                   Atrac9CodecInfo& codec) {
	return Ajm::AjmAt9GetCodecInfo(config.data(), codec);
}

static int Ngs2ParseAtrac9Riff(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
	static constexpr uint8_t ATRAC9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36,
	                                            0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65,
	                                            0x4f, 0x8c, 0x83, 0x6c};
	const auto* bytes = static_cast<const uint8_t*>(data);
	if (bytes == nullptr || data_size < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (!Ngs2FourCcEquals(bytes, "RIFF") || !Ngs2FourCcEquals(bytes + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint64_t riff_end64 = 8ull + Ngs2ReadLe32(bytes + 4);
	if (riff_end64 < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto riff_end = static_cast<size_t>(riff_end64);

	const uint8_t* format           = nullptr;
	const uint8_t* fact             = nullptr;
	size_t         waveform_offset  = 0;
	uint32_t       waveform_size    = 0;

	for (size_t offset = 12; offset + 8 <= std::min(riff_end, data_size);) {
		const auto* chunk       = bytes + offset;
		const auto  chunk_size  = static_cast<size_t>(Ngs2ReadLe32(chunk + 4));
		const auto  payload     = offset + 8;
		const auto  is_data     = Ngs2FourCcEquals(chunk, "data");
		const uint64_t next    = static_cast<uint64_t>(payload) + chunk_size + (chunk_size & 1u);
		if (next > riff_end || (next > data_size && !is_data)) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}

		if (Ngs2FourCcEquals(chunk, "fmt ")) {
			if (chunk_size < 52) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			format = bytes + payload;
		} else if (Ngs2FourCcEquals(chunk, "fact")) {
			if (chunk_size < 12) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			fact = bytes + payload;
		} else if (is_data) {
			if (payload > std::numeric_limits<uint32_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			waveform_offset = payload;
			waveform_size   = static_cast<uint32_t>(chunk_size);
		}

		if (next > data_size) {
			break;
		}
		offset = static_cast<size_t>(next);
	}

	if (format == nullptr || fact == nullptr || waveform_offset == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	if (Ngs2ReadLe16(format) != 0xfffe ||
	    std::memcmp(format + 24, ATRAC9_GUID, sizeof(ATRAC9_GUID)) != 0) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {};
	std::memcpy(config.data(), format + 44, config.size());
	Atrac9CodecInfo codec {};
	const bool valid_codec = Ngs2GetAtrac9CodecInfo(config, codec);
	const uint64_t frame_samples =
	    static_cast<uint64_t>(codec.frameSamples) * static_cast<uint64_t>(codec.framesInSuperframe);
	if (!valid_codec || codec.channels != Ngs2ReadLe16(format + 2) ||
	    codec.samplingRate != static_cast<int>(Ngs2ReadLe32(format + 4)) ||
	    codec.superframeSize != Ngs2ReadLe16(format + 12) ||
	    frame_samples != Ngs2ReadLe16(format + 18) ||
	    frame_samples > std::numeric_limits<uint32_t>::max()) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	info->format.waveform_type = NGS2_WAVEFORM_TYPE_ATRAC9;
	info->format.num_channels  = static_cast<uint32_t>(codec.channels);
	info->format.sample_rate   = static_cast<uint32_t>(codec.samplingRate);
	info->format.config_data = (static_cast<uint32_t>(config[0]) << 24u) |
	                           (static_cast<uint32_t>(config[1]) << 16u) |
	                           (static_cast<uint32_t>(config[2]) << 8u) | config[3];
	info->data_offset              = static_cast<uint32_t>(waveform_offset);
	info->data_size                = waveform_size;
	info->num_samples              = Ngs2ReadLe32(fact);
	info->audio_unit_size          = static_cast<uint32_t>(codec.superframeSize /
	                                                       codec.framesInSuperframe);
	info->num_audio_unit_samples   = static_cast<uint32_t>(codec.frameSamples);
	info->num_audio_unit_per_frame = static_cast<uint32_t>(codec.framesInSuperframe);
	info->audio_frame_size         = static_cast<uint32_t>(codec.superframeSize);
	info->num_audio_frame_samples  = static_cast<uint32_t>(frame_samples);
	info->num_delay_samples        = Ngs2ReadLe32(fact + 4);
	info->num_blocks               = 1;
	info->blocks[0].data_offset    = waveform_offset;
	info->blocks[0].data_size      = waveform_size;
	info->blocks[0].num_skip_samples = Ngs2ReadLe32(fact + 8);
	info->blocks[0].num_samples      = info->num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size,
                                        Ngs2WaveformInfo* info) {
	PRINT_NAME();
	LOGF("\t data = 0x%016" PRIx64 ", data_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(data), static_cast<uint64_t>(data_size));

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}

	std::memset(info, 0, sizeof(Ngs2WaveformInfo));
	return Ngs2ParseAtrac9Riff(data, data_size, info);
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block) {
	PRINT_NAME();
	LOGF("\t format = 0x%016" PRIx64 ", sample_pos = %" PRIu32 ", num_samples = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(format), sample_pos, num_samples);

	if (block == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}

	std::memset(block, 0, sizeof(Ngs2WaveformBlock));
	if (format == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	// CapyPS5: `debug.capy.ngs2_at9_delay 0` computes ATRAC9 blocks as before the decoder delay
	// was counted (no delay, and the channel and rate checks).
	static Common::DebugFlags::CachedValue at9_delay("debug.capy.ngs2_at9_delay", 1);
	if (format->waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9 && at9_delay.Get() == 0) {
		std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {
		    static_cast<uint8_t>(format->config_data >> 24u),
		    static_cast<uint8_t>(format->config_data >> 16u),
		    static_cast<uint8_t>(format->config_data >> 8u),
		    static_cast<uint8_t>(format->config_data)};
		Atrac9CodecInfo codec {};
		if (!Ngs2GetAtrac9CodecInfo(config, codec) ||
		    codec.channels != format->num_channels || codec.samplingRate != format->sample_rate) {
			return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
		}
		const uint64_t frame_samples = uint64_t(codec.frameSamples) * codec.framesInSuperframe;
		const uint64_t skip          = sample_pos % frame_samples;
		block->data_offset = uint64_t(sample_pos / frame_samples) * codec.superframeSize;
		block->data_size   = num_samples == 0
		                         ? 0
		                         : ((skip + num_samples + frame_samples - 1) / frame_samples) *
		                               codec.superframeSize;
		block->num_skip_samples = num_samples == 0 ? 0 : static_cast<uint32_t>(skip);
	} else if (format->waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
		// The superframe of one stream (sync 0xfe) or of several (sync 0x30), in 32-bit math.
		const uint32_t config = format->config_data;
		uint32_t       frames = 0, superframe_size = 0;
		if (config >> 24u == 0xfe) {
			frames          = 1u << ((config >> 3u) & 3u);
			superframe_size = (((config >> 5u) & 0x7ffu) + 1u) * frames;
		} else if (config >> 24u == 0x30) {
			const uint32_t streams = ((config >> 14u) & 0x3fu) + 1u;
			frames                 = 1u << (config & 3u);
			superframe_size        = streams * (((config >> 2u) & 0x7ffu) + 1u) * frames;
		}
		// Frame samples by sample rate index; only 12, 24 and 48 kHz have frames.
		constexpr std::array<uint32_t, 16> FRAME_SAMPLES = {0, 64, 0, 0, 128, 0, 0, 256};

		const uint32_t frame_samples      = FRAME_SAMPLES[(config >> 20u) & 0xfu];
		const uint32_t superframe_samples = frame_samples * frames;
		if (superframe_samples == 0) {
			return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		// Decoded samples lag the superframes by one frame.
		const uint32_t first = sample_pos / superframe_samples;
		const uint32_t last  = sample_pos + frame_samples + num_samples + superframe_samples - 1;

		block->data_offset      = first * superframe_size;
		block->data_size        = (last / superframe_samples - first) * superframe_size;
		block->num_skip_samples = sample_pos + frame_samples - first * superframe_samples;
	} else {
		// PCM: 8/16/24/32-bit integer and 32/64-bit float, little or big endian (The Binding of
		// Isaac streams 16-bit PCM and sent the zeroed block on, which played nothing).
		uint32_t bytes_per_sample = 0;
		switch (format->waveform_type) {
			case 0x10:
			case 0x11: bytes_per_sample = 1; break;
			case 0x12:
			case 0x13: bytes_per_sample = 2; break;
			case 0x14:
			case 0x15: bytes_per_sample = 3; break;
			case 0x16:
			case 0x17:
			case 0x18:
			case 0x19: bytes_per_sample = 4; break;
			case 0x1a:
			case 0x1b: bytes_per_sample = 8; break;
			default: return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		const uint64_t frame_bytes = uint64_t(bytes_per_sample) * format->num_channels;
		block->data_offset         = uint64_t(sample_pos) * frame_bytes;
		block->data_size           = uint64_t(num_samples) * frame_bytes;
	}
	block->num_samples = num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", num_speakers = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), num_speakers);

	EXIT_NOT_IMPLEMENTED(work == nullptr);

	std::memset(work, 0, sizeof(Ngs2PanWork));
	work->unit_angle   = unit_angle;
	work->num_speakers = std::min<uint32_t>(num_speakers, 8);
	if (speaker_angles != nullptr) {
		for (uint32_t i = 0; i < work->num_speakers; i++) {
			work->speaker_angles[i] = speaker_angles[i];
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params,
                                         uint32_t num_params, uint32_t matrix_format,
                                         float* out_volume_matrix) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", params = 0x%016" PRIx64 ", num_params = %" PRIu32
	     ", matrix_format = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), reinterpret_cast<uint64_t>(params), num_params,
	     matrix_format);

	EXIT_NOT_IMPLEMENTED(out_volume_matrix == nullptr && num_params != 0);

	const auto channels = (matrix_format == 0 ? 2u : std::min<uint32_t>(matrix_format, 8));
	for (uint32_t p = 0; p < num_params; p++) {
		for (uint32_t c = 0; c < channels; c++) {
			out_volume_matrix[p * channels + c] = (c == 0 ? 1.0f : 0.0f);
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_listener_param == nullptr);

	std::memset(out_listener_param, 0, sizeof(Ngs2GeomListenerParam));
	out_listener_param->orient_front.z = 1.0f;
	out_listener_param->orient_up.y    = 1.0f;
	out_listener_param->sound_speed    = 343.0f;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_source_param == nullptr);

	std::memset(out_source_param, 0, sizeof(Ngs2GeomSourceParam));
	out_source_param->direction.z                = 1.0f;
	out_source_param->cone.inner_level           = 1.0f;
	out_source_param->cone.inner_angle           = 360.0f;
	out_source_param->cone.outer_level           = 1.0f;
	out_source_param->cone.outer_angle           = 360.0f;
	out_source_param->rolloff.model              = 0;
	out_source_param->rolloff.max_distance       = 1000000.0f;
	out_source_param->rolloff.rolloff_factor     = 1.0f;
	out_source_param->rolloff.reference_distance = 1.0f;
	out_source_param->doppler_factor             = 1.0f;
	out_source_param->fbw_level                  = 1.0f;
	out_source_param->lfe_level                  = 1.0f;
	out_source_param->max_level                  = 1.0f;
	out_source_param->min_level                  = 0.0f;
	out_source_param->num_speakers               = 2;
	out_source_param->matrix_format              = 2;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param,
                                       Ngs2GeomListenerWork* out_work, uint32_t flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param == nullptr);
	EXIT_NOT_IMPLEMENTED(out_work == nullptr);

	std::memset(out_work, 0, sizeof(Ngs2GeomListenerWork));
	for (uint32_t i = 0; i < 4; i++) {
		out_work->matrix[i][i] = 1.0f;
	}
	out_work->velocity    = param->velocity;
	out_work->sound_speed = (param->sound_speed > 0.0f ? param->sound_speed : 343.0f);
	out_work->coordinate  = flags & 0x1u;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomApply(const Ngs2GeomListenerWork* listener,
                                const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib,
                                uint32_t flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(listener == nullptr);
	EXIT_NOT_IMPLEMENTED(source == nullptr);
	EXIT_NOT_IMPLEMENTED(out_attrib == nullptr);

	std::memset(out_attrib, 0, sizeof(Ngs2GeomAttribute));
	out_attrib->pitch_ratio         = 1.0f;
	out_attrib->a3d_attrib.position = source->position;
	out_attrib->a3d_attrib.volume   = std::max(source->min_level, source->max_level);

	const auto channels =
	    std::min<uint32_t>((source->matrix_format == 0 ? 2u : source->matrix_format), 8);
	const auto level = (source->max_level > 0.0f ? source->max_level : 1.0f);
	for (uint32_t ch = 0; ch < channels; ch++) {
		out_attrib->level[ch * 8 + ch] = level;
	}

	return OK;
}

// A patch destination may be a rack handle: Dead Cells sends its samplers and reverb to the
// mastering rack itself, which stands for that rack's first voice.
static Ngs2VoiceInternal* Ngs2PatchDestination(uintptr_t handle) {
	if (handle == 0) {
		return nullptr;
	}
	auto* rack = Ngs2FindRack(handle);
	if (rack == nullptr) {
		return reinterpret_cast<Ngs2VoiceInternal*>(handle);
	}
	return rack->option.common.max_voices == 0 ? nullptr
	                                           : reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle) {
	PRINT_NAME();

	LOGF("\t voice_id = %u\n", voice_id);

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	if (handle == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (voice_id >= rack->option.common.max_voices) {
		// CapyPS5: `debug.capy.ngs2_voice_index_fallback 1` hands out the last voice instead, as
		// before this change.
		static Common::DebugFlags::CachedValue fallback("debug.capy.ngs2_voice_index_fallback", 0);
		if (fallback.Get() == 0 || rack->option.common.max_voices == 0) {
			return NGS2_ERROR_INVALID_VOICE_INDEX;
		}
		LOGF("\t warning: voice_id %u >= max_voices %u, using last available stub voice\n",
		     voice_id, rack->option.common.max_voices);
		voice_id = rack->option.common.max_voices - 1;
	}

	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	*handle      = reinterpret_cast<uintptr_t>(voices + voice_id);

	return OK;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param_list == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	const auto* param = param_list;

	for (;;) {
		auto rack_id = param->id >> 16u;

		// Bit 15 of a custom rack id is bit 7 of its module id.
		EXIT_NOT_IMPLEMENTED(rack_id != 0x3000 && rack_id != 0x4000 &&
		                     ((param->id >> 15u) & 0x1u) != 0);

		switch (rack_id) {
			case 0x0000: {
				auto cid = param->id & 0x7fffu;
				switch (cid) {
					case 0x0001: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceMatrixLevelsParam));
						const auto* ml = reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						voice->SetMatrix(ml->matrix_id, ml->levels, ml->num_levels);
						break;
					}
					case 0x0002: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortVolumeParam));
						const auto* volume =
						    reinterpret_cast<const Ngs2VoicePortVolumeParam*>(param);
						voice->SetVolume(volume->port, volume->level);
						break;
					}
					case 0x0003: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortMatrixParam));
						const auto* pm = reinterpret_cast<const Ngs2VoicePortMatrixParam*>(param);
						voice->SetPortMatrix(pm->port, pm->matrix_id);
						break;
					}
					case 0x0004: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortDelayParam));
						const auto* delay = reinterpret_cast<const Ngs2VoicePortDelayParam*>(param);
						break;
					}
					case 0x0005: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePatchParam));
						const auto* patch = reinterpret_cast<const Ngs2VoicePatchParam*>(param);
						EXIT_NOT_IMPLEMENTED(patch->port >= voice->ports.size());
						voice->ports[patch->port].dest = Ngs2PatchDestination(patch->dest_handle);
						voice->ports[patch->port].input = patch->dest_input_id;
						break;
					}
					case 0x0006: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceEventParam));
						const auto* event = reinterpret_cast<const Ngs2VoiceEventParam*>(param);
						voice->SetEvent(event->event_id);
						break;
					}
					case 0x0007: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceCallbackParam));
						const auto* callback =
						    reinterpret_cast<const Ngs2VoiceCallbackParam*>(param);
						voice->callback       = callback->callback;
						voice->callback_data  = callback->callback_data;
						voice->callback_flags = callback->flags;
						break;
					}
					default: EXIT("unknown id: 0x%04" PRIx32 "\n", cid);
				}
				break;
			}
			case 0x2000: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer);
				if ((param->id & 0xffffu) == 0) {
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2SubmixerVoiceSetupParam));
					const auto& setup =
					    *reinterpret_cast<const Ngs2SubmixerVoiceSetupParam*>(param);
					EXIT_NOT_IMPLEMENTED(
					    setup.num_io_channels == 0 ||
					    setup.num_io_channels > voice->rack->option.submixer.max_channels ||
					    setup.flags != 0);
					voice->ResetSetupState();
					voice->channels = setup.num_io_channels;
				}
				break;
			}
			case 0x2001: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb);
				// Setup: input and output channel counts. The reverb itself is not simulated: the
				// voice passes its inputs through (Animal Well routes all its sound through one, so
				// leaving it without channels silenced the game). Effect parameters are ignored.
				if ((param->id & 0xffffu) == 0 && param->size >= sizeof(*param) + 2 * sizeof(uint32_t)) {
					const auto* values  = reinterpret_cast<const uint32_t*>(param + 1);
					const auto  max     = std::max(voice->rack->option.reverb.max_channels, 2u);
					const auto  outputs = values[1] != 0 && values[1] <= max ? values[1] : values[0];
					if (outputs != 0 && outputs <= max) {
						voice->ResetSetupState();
						voice->channels = outputs;
					}
				}
				break;
			}
			case 0x3000: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Mastering);
				const auto  cid       = param->id & 0xffffu;
				const auto* values    = reinterpret_cast<const uint32_t*>(param + 1);
				auto&       mastering = *voice->mastering;
				if (cid > 6) {
					return NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
				}
				if (param->size < (cid == 1 ? sizeof(Ngs2VoiceMatrixLevelsParam)
				                            : sizeof(Ngs2VoiceParamHeader) + 8)) {
					return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
				}
				switch (cid) {
					case 0:
						if (values[0] - 1 >= voice->rack->option.mastering.max_channels) {
							return NGS2_ERROR_INVALID_NUM_CHANNELS;
						}
						EXIT_NOT_IMPLEMENTED(values[0] > 8);
						// Setup restores the default levels, disables the LFE filter, the limiter,
						// the gain and the peak meters, and clears the limiter and meter states;
						// ramps, filter history, the limiter gain and the meter window go on.
						mastering.levels          = NGS2_MASTERING_DEFAULT_LEVELS;
						mastering.lfe_enabled     = false;
						mastering.limiter_enabled = false;
						mastering.limiter_state   = {};
						mastering.gain_enabled    = false;
						mastering.meters_enabled  = false;
						mastering.meter_channels  = 0;
						voice->channels           = values[0];
						voice->output_id          = 0;
						break;
					case 1: {
						const auto* matrix =
						    reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						const auto type = matrix->matrix_id;
						if (type >= 16) {
							return NGS2_ERROR_INVALID_MATRIX_TYPE;
						}
						// Levels are copied unclamped from the type's field on, so extra levels
						// overwrite the following fields.
						// Type t converts channels[t / 4] into channels[t % 4] channels.
						constexpr std::array<uint32_t, 4> channels = {1, 2, 6, 8};
						const auto                        offset =
						    Ngs2MasteringLevelsOffset(channels[type / 4], channels[type % 4]);
						const size_t count = static_cast<uint16_t>(matrix->num_levels);
						std::copy_n(matrix->levels,
						            std::min(count, mastering.levels.size() - offset),
						            mastering.levels.begin() + offset);
						break;
					}
					case 2:
						// The enable flag is clamped to 1 and fc to 80..120 Hz; up to 80 Hz selects
						// the 80 Hz filter, above it the 120 Hz one.
						mastering.lfe_enable_flag = values[0] != 0;
						Ngs2SetMasteringLfe(mastering, values[1] <= 80 ? 80u : 120u,
						                    voice->rack->ngs->option.sample_rate);
						mastering.lfe_enabled = true;
						break;
					case 3: {
						const auto* levels = reinterpret_cast<const float*>(values);
						const auto  rate   = voice->rack->ngs->option.sample_rate;
						// The threshold is clamped to 0..1; a NaN threshold never limits.
						mastering.limiter_enable_flag = values[0] != 0;
						mastering.limiter_threshold   = std::clamp(levels[1], 0.0f, 1.0f);
						mastering.limiter_attack      = Ngs2LimiterCoefficient(rate, 0.01f);
						mastering.limiter_release     = Ngs2LimiterCoefficient(rate, 0.1f);
						mastering.limiter_enabled     = true;
						break;
					}
					case 4: { // gain: full-band level, LFE level
						const auto* levels = reinterpret_cast<const float*>(values);
#if defined(__ANDROID__)
						static std::atomic<uint32_t> gain_logs {0};
						if (gain_logs.fetch_add(1) < 8) {
							const auto* raw = reinterpret_cast<const uint32_t*>(param + 1);
							__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Ngs2",
							                    "mastering gain size=%u raw %08x %08x %08x %08x",
							                    static_cast<unsigned>(param->size), raw[0], raw[1], raw[2], raw[3]);
						}
#endif
						// CapyPS5: a non-finite level keeps the previous gain instead of silencing
						// the output.
						if (std::isfinite(levels[0]) && std::isfinite(levels[1])) {
							mastering.gain         = {std::clamp(levels[0], -32.0f, 32.0f),
							                          std::clamp(levels[1], -32.0f, 32.0f)};
							mastering.gain_enabled = true;
						}
						break;
					}
					case 5: voice->output_id = std::min(values[0], 32u); break;
					case 6:
						if (values[0] > 2) {
							return NGS2_ERROR_INVALID_PEAK_METER_DETECT_TYPE;
						}
						mastering.meter_detect_type = values[0];
						mastering.meters_enabled    = true;
						break;
				}
				break;
			}
			case 0x4000: {
				EXIT_NOT_IMPLEMENTED(!Ngs2RackIsCustom(voice->rack->type));
				// The id names module `index` and its module id; bits 5-7 are not read.
				const auto  index   = param->id & 0x1fu;
				const auto& modules = voice->rack->option.custom_sampler.custom_rack_option.module;
				if (index >= voice->modules.size() ||
				    (param->id >> 8u & 0xffu) != modules[index].module_id) {
					return NGS2_ERROR_INVALID_VOICE_PARAM_ID;
				}
				const auto module_id = modules[index].module_id;
				auto&      module    = voice->modules[index];
				if (module_id != 0x17 && module_id != 0x19 && module_id != 0x1f) {
					if (!module.control_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom control 0x%08" PRIx32 " is ignored\n",
						    param->id));
						module.control_warned = true;
					}
#if defined(__ANDROID__)
					static Common::DebugFlags::CachedValue ngs2_log("debug.capy.ngs2_log", 0);
					static std::atomic<uint32_t> ignored_logs {0};
					if (ngs2_log.Get() != 0 && ignored_logs.fetch_add(1) < 64) {
						const auto* raw = reinterpret_cast<const uint32_t*>(param + 1);
						__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Ngs2",
						                    "ignored control 0x%08x voice=%p size=%u data=%08x %08x "
						                    "%08x %08x %08x %08x %08x %08x",
						                    param->id, static_cast<void*>(voice),
						                    static_cast<unsigned>(param->size), raw[0], raw[1],
						                    raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
					}
#endif
					break;
				}
				if (module_id == 0x17) {
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceParamHeader) +
					                                       37 * 37 * sizeof(float));
					const auto* levels = reinterpret_cast<const float*>(param + 1);
					module.channel_matrix.resize(37 * 37);
					std::transform(levels, levels + module.channel_matrix.size(),
					               module.channel_matrix.begin(),
					               [](float level) { return std::clamp(level, -32.0f, 32.0f); });
					break;
				}
				if (module_id == 0x19) {
					struct FxParam {
						Ngs2VoiceParamHeader     header;
						decltype(module.handler) handler;
						std::array<uintptr_t, 3> user_data;
					};
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(FxParam));
					const auto& fx = *reinterpret_cast<const FxParam*>(param);
					// Enabling the handler requests a reset; later updates mark user data dirty.
					module.flags |= module.handler == nullptr ? 1u : 2u;
					module.handler   = fx.handler;
					module.user_data = fx.user_data;
					break;
				}
				struct Fx2Param {
					Ngs2VoiceParamHeader header;
					const void*          data;
					size_t               size;
				};
				const auto& fx = *reinterpret_cast<const Fx2Param*>(param);
				EXIT_NOT_IMPLEMENTED(voice->rack->fx[index].control != 0 ||
				                     fx.size != module.param.size());
				std::memcpy(module.param.data(), fx.data, fx.size);
				module.flags |= 2;
				break;
			}
			case 0x1000:
			case 0x4001: {
				EXIT_NOT_IMPLEMENTED(
				    voice->rack->type !=
				    (rack_id == 0x1000 ? Ngs2RackType::Sampler : Ngs2RackType::CustomSampler));
				switch (param->id & 0xffffu) {
					case 0: {
						const auto& format =
						    *reinterpret_cast<const Ngs2WaveformFormat*>(param + 1);
#if defined(__ANDROID__)
						static Common::DebugFlags::CachedValue ngs2_log("debug.capy.ngs2_log", 0);
						static std::atomic<uint32_t> format_logs {0};
						if (ngs2_log.Get() != 0 && format_logs.fetch_add(1) < 64) {
							__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Ngs2",
							                    "voice %p rack=0x%x format type=0x%x ch=%u rate=%u "
							                    "config=%08x",
							                    static_cast<void*>(voice), rack_id,
							                    format.waveform_type, format.num_channels,
							                    format.sample_rate, format.config_data);
						}
#endif
						voice->SetupSampler(format);
						break;
					}
					case 1: {
						struct BlocksParam {
							Ngs2VoiceParamHeader     header;
							const uint8_t*           data;
							uint32_t                 flags, count;
							const Ngs2WaveformBlock* blocks;
						};
						if (param->size < sizeof(BlocksParam)) {
							return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
						}
						const auto& blocks = *reinterpret_cast<const BlocksParam*>(param);
						if (blocks.count != 0 && blocks.blocks == nullptr) {
							return NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
						}
						const auto result = voice->AddWaveformBlocks(
						    blocks.data, blocks.flags, blocks.count, blocks.blocks);
						if (result != OK) {
							return result;
						}
						break;
					}
					case 4: {
						for (auto& block: voice->blocks) {
							if (block.info.num_repeats != 0) {
								block.info.num_repeats = 0;
								break;
							}
						}
						break;
					}
						case 5: {
							const float ratio = *reinterpret_cast<const float*>(param + 1);
						EXIT_NOT_IMPLEMENTED(!std::isfinite(ratio));
						const float clamped_ratio = std::clamp(ratio, 0.0f, 4.0f);
						voice->sample_step = static_cast<uint64_t>(std::llround(
							    static_cast<double>(voice->sample_rate) * clamped_ratio * 4294967296.0));
						break;
					}
					case 0xa: {
						EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler);
						if (param->size <
						    sizeof(Ngs2VoiceParamHeader) + sizeof(Ngs2SamplerFilterParam)) {
							return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
						}
						const auto& filter =
						    *reinterpret_cast<const Ngs2SamplerFilterParam*>(param + 1);
						if (filter.index >= voice->rack->option.sampler.max_filters) {
							return NGS2_ERROR_INVALID_VOICE_PARAM_ID;
						}
						if (filter.filter.type > 11) {
							return NGS2_ERROR_INVALID_FILTER_TYPE;
						}
						if (filter.location >= voice->ports.size() + 2) {
							return NGS2_ERROR_INVALID_MODULE_LOCATION;
						}
						// CapyPS5: `debug.capy.ngs2_filters 0` ignores the sampler filters, which
						// the previous (misread) layout mostly left disabled.
						static Common::DebugFlags::CachedValue filters_flag("debug.capy.ngs2_filters", 1);
						if (filters_flag.Get() == 0) {
							break;
						}
						if (voice->filters.size() <= filter.index) {
							voice->filters.resize(filter.index + 1);
						}
						// A change keeps the history.
						auto& target    = voice->filters[filter.index];
						target.location = filter.location;
						target.history.resize(voice->channels);
						target.Design(filter.filter, voice->rack->ngs->option.sample_rate);
						break;
					}
					default: EXIT("unsupported sampler control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			case 0x4002: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer ||
				                     (param->id & 0xffffu) != 0);
				const auto* channels   = reinterpret_cast<const uint32_t*>(param + 1);
				EXIT_NOT_IMPLEMENTED(channels[0] != channels[1]);
				voice->channels = channels[0];
				break;
			}
			case 0x4003: {
				// SceNgs2CustomMasteringVoiceSetupParam {numInputChannels, flags} or
				// SceNgs2CustomMasteringVoiceOutputParam {outputId, reserved}.
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomMastering ||
				                     param->size != sizeof(Ngs2VoiceParamHeader) + 8u);
				const auto* values = reinterpret_cast<const uint32_t*>(param + 1);
				switch (param->id & 0xffffu) {
					case 0:
						EXIT_NOT_IMPLEMENTED(values[0] == 0 ||
						                     values[0] > voice->rack->option.common.max_voice_channels ||
						                     values[1] != 0);
						voice->channels = values[0];
						break;
					case 1: voice->output_id = values[0]; break;
					default:
						EXIT("unsupported custom mastering control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
		}

		if (param->next == 0) {
			break;
		}
		param = reinterpret_cast<const Ngs2VoiceParamHeader*>(reinterpret_cast<uintptr_t>(param) +
		                                                      param->next);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands,
                                       size_t num_commands) {
	struct Command {
		uint32_t id;
		uint8_t  flags, type;
		uint16_t count;
		union {
			uint32_t     u;
			int32_t      i;
			float        f;
			const float* levels;
		} value;
	};
	auto*             voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);
	Common::LockGuard lock(voice->rack->ngs->mutex);
	const auto*       params = static_cast<const Command*>(commands);
	for (size_t i = 0; i < num_commands; ++i) {
		const auto& p     = params[i];
		const auto  index = p.id >> 24;
		switch (p.id & 0xffffffu) {
			case 2:
				EXIT_NOT_IMPLEMENTED(p.type != 4);
				voice->SetEvent(p.value.u);
				break;
			case 5:
				EXIT_NOT_IMPLEMENTED(p.type != 0x11);
				voice->SetMatrix(index, p.value.levels, p.count);
				break;
			case 6:
				EXIT_NOT_IMPLEMENTED(p.type != 1);
				voice->SetVolume(index, p.value.f);
				break;
			case 7:
				EXIT_NOT_IMPLEMENTED(p.type != 3);
				voice->SetPortMatrix(index, p.value.i);
				break;
			default: EXIT("unknown command: 0x%08" PRIx32 "\n", p.id);
		}
	}
	return OK;
}

struct Ngs2VoicePortInfo {
	int32_t   matrix_id;
	float     volume;
	uint32_t  num_delay_samples;
	uint32_t  dest_input_id;
	uintptr_t dest_handle;
};
static_assert(sizeof(Ngs2VoicePortInfo) == 24);

constexpr int32_t NGS2_ERROR_INVALID_VOICE_HANDLE = static_cast<int32_t>(0x804a0230u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_PORT   = static_cast<int32_t>(0x804a0235u);

// Animal Well queries where each voice sends its output.
int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, Ngs2VoicePortInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (voice_handle == 0) {
		return NGS2_ERROR_INVALID_VOICE_HANDLE;
	}
	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);
	if (port >= voice->ports.size()) {
		return NGS2_ERROR_INVALID_VOICE_PORT;
	}
	const auto& state       = voice->ports[port];
	*info                   = {};
	info->matrix_id         = state.matrix;
	info->volume            = state.volume;
	info->dest_input_id     = state.input;
	info->dest_handle       = reinterpret_cast<uintptr_t>(state.dest);
	return OK;
}

// The rack info layout is not known; report it zeroed instead of leaving the caller's buffer as
// it was.
int KYTY_SYSV_ABI Ngs2RackGetInfo(uintptr_t rack_handle, void* info, size_t info_size) {
	PRINT_NAME();

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	LOGF("\t rack = 0x%016" PRIx64 ", info_size = %" PRIu64 "\n", static_cast<uint64_t>(rack_handle),
	     static_cast<uint64_t>(info_size));
	std::memset(info, 0, info_size);
	return OK;
}

template <typename State>
static void Ngs2FillSamplerState(const Ngs2VoiceInternal& voice, State& state) {
	state.voice_state.state_flags = Ngs2GetStateFlags(&voice);
	state.num_decoded_samples     = voice.decoded_samples;
	state.decoded_data_size       = voice.decoded_bytes;
	state.waveform_data           = voice.WaveformData();
	state.user_data               = voice.blocks.empty() ? 0 : voice.blocks.front().info.user_data;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);
	if (voice_handle == 0) {
		// Rayman Legends (PS4) polls voices it has not created yet.
		return NGS2_ERROR_INVALID_VOICE_HANDLE;
	}

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	switch (voice->rack->type) {
		case Ngs2RackType::Submixer: {
			EXIT_NOT_IMPLEMENTED(state_size != sizeof(Ngs2SubmixerVoiceState));
			auto* submixer                    = reinterpret_cast<Ngs2SubmixerVoiceState*>(state);
			*submixer                         = {};
			submixer->voice_state.state_flags = Ngs2GetStateFlags(voice);
			break;
		}
		case Ngs2RackType::Mastering: {
			if (state_size < sizeof(Ngs2MasteringVoiceState)) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			const auto& modules                = *voice->mastering;
			auto*       mastering              = reinterpret_cast<Ngs2MasteringVoiceState*>(state);
			*mastering                         = {};
			mastering->voice_state.state_flags = Ngs2GetStateFlags(voice);
			mastering->limiter_peak_level      = modules.limiter_state[0];
			mastering->limiter_press_level     = modules.limiter_state[1];
			std::copy_n(modules.meter_peaks[0].begin(), modules.meter_channels,
			            mastering->input_peak_height);
			std::copy_n(modules.meter_peaks[1].begin(), modules.meter_channels,
			            mastering->output_peak_height);
			break;
		}
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: {
			const bool sampler = voice->rack->type == Ngs2RackType::CustomSampler;
			const auto configured_size =
			    voice->rack->option.custom_sampler.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < (sampler ? sizeof(Ngs2CustomSamplerVoiceState)
			                                                : sizeof(Ngs2CustomMasteringVoiceState)));
			if (state_size < configured_size) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			std::memset(state, 0, configured_size);
			if (sampler) {
				Ngs2FillSamplerState(*voice, *reinterpret_cast<Ngs2CustomSamplerVoiceState*>(state));
			} else {
				state->state_flags = Ngs2GetStateFlags(voice);
			}
			break;
		}
		case Ngs2RackType::Sampler: {
			if (state_size != sizeof(Ngs2SamplerVoiceState)) {
				LOGF("\t warning: sampler state_size = 0x%016" PRIx64 ", expected 0x%016" PRIx64
				     "\n",
				     static_cast<uint64_t>(state_size),
				     static_cast<uint64_t>(sizeof(Ngs2SamplerVoiceState)));
			}
			std::memset(state, 0, state_size);

			state->state_flags = Ngs2GetStateFlags(voice);
			if (state_size < sizeof(Ngs2SamplerVoiceState)) {
				break;
			}

			auto* sampler                = reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler->envelope_height     = 1.0f;
			sampler->peak_height         = 0.0f;
			sampler->reserved            = 0;
			Ngs2FillSamplerState(*voice, *sampler);
			break;
		}
		default: EXIT("unknown type: %s\n", magic_enum::enum_name(voice->rack->type));
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state_flags == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*state_flags = Ngs2GetStateFlags(voice);

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t query_id, void* info,
                                     size_t info_size) {
	PRINT_NAME();

	struct SetupInfo {
		uint32_t channels, flags;
	};
	EXIT_NOT_IMPLEMENTED(voice_handle == 0 || info == nullptr);
	EXIT_NOT_IMPLEMENTED(query_id != 0x4001 || info_size != sizeof(SetupInfo));

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*static_cast<SetupInfo*>(info) = {voice->channels, 0};

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle) {
	PRINT_NAME();

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	ngs->user_lock.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle) {
	PRINT_NAME();

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	ngs->user_lock.Unlock();

	return OK;
}

} // namespace Ngs2

} // namespace Libs::Audio
