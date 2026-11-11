// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/driverQuirks.h"

#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/HostFeatures.h"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cinttypes>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Libs::Graphics {

namespace {

// A driver bug or limitation and the workaround it gets. Add one entry per workaround; the
// detection decides the default, the property forces it either way.
struct DriverQuirk {
	const char* name;        // short id, shown in logs
	const char* property;    // debug.capy.quirk_<name>: -1 auto, 0 off, 1 on
	const char* description; // what the driver gets wrong and what the workaround does
	bool (*detect)(const DriverInfo& info);
	bool* target;            // the switch the renderer or the shader recompiler reads
};

bool IsQualcommProprietary(const DriverInfo& info) {
	return info.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
}

const std::array g_quirks {
    DriverQuirk {
        "kill_exports_depth", "debug.capy.quirk_kill_exports_depth",
        "pixel shaders that kill export gl_FragCoord.z: the stock Adreno LRZ dropped their "
        "fragments in 8x8 blocks (God of War Ragnarok foliage, Rogue Prince of Persia)",
        IsQualcommProprietary, &ShaderRecompiler::g_host_features.kill_exports_depth},
};

std::string g_summary = "driver quirks not applied yet";

const char* DriverIdName(uint32_t id) {
	switch (id) {
		case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "Qualcomm proprietary";
		case VK_DRIVER_ID_MESA_TURNIP: return "Mesa Turnip";
		case VK_DRIVER_ID_ARM_PROPRIETARY: return "ARM proprietary";
		case VK_DRIVER_ID_SAMSUNG_PROPRIETARY: return "Samsung proprietary";
		case VK_DRIVER_ID_IMAGINATION_PROPRIETARY: return "Imagination proprietary";
		case VK_DRIVER_ID_MESA_PANVK: return "Mesa PanVK";
		case VK_DRIVER_ID_MESA_RADV: return "Mesa RADV";
		case VK_DRIVER_ID_AMD_PROPRIETARY: return "AMD proprietary";
		case VK_DRIVER_ID_NVIDIA_PROPRIETARY: return "NVIDIA proprietary";
		case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS: return "Intel proprietary";
		case VK_DRIVER_ID_MESA_LLVMPIPE: return "Mesa llvmpipe";
		default: return "unknown";
	}
}

void Log(const char* text) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Quirks", "%s", text);
#endif
	LOGF("Quirks: %s\n", text);
}

} // namespace

void ApplyDriverQuirks(const DriverInfo& info) {
	// Qualcomm's driverInfo spans several lines (build, date, compiler): keep it on one.
	std::string driver_info;
	for (const char c: info.driver_info) {
		if (c == '\n' || c == '\r') {
			if (!driver_info.empty() && driver_info.back() != ' ') driver_info += " | ";
		} else {
			driver_info += c;
		}
	}
	char line[512];
	std::snprintf(line, sizeof(line), "%s (id %u) \"%s\" %s, version 0x%08" PRIx32
	              ", vendor 0x%04" PRIx32 " device 0x%08" PRIx32 ", %s",
	              DriverIdName(info.driver_id), info.driver_id, info.driver_name.c_str(),
	              driver_info.c_str(), info.driver_version, info.vendor_id, info.device_id,
	              info.device_name.c_str());
	g_summary = line;
	g_summary += "; quirks:";
	Log(line);
	bool any = false;
	for (const auto& quirk: g_quirks) {
		const bool detected = quirk.detect(info);
		Common::DebugFlags::CachedValue forced(quirk.property, -1);
		const auto value   = forced.Get();
		const bool enabled = value < 0 ? detected : value != 0;
		*quirk.target      = enabled;
		std::snprintf(line, sizeof(line), "%s %s (%s): %s", enabled ? "ON " : "off", quirk.name,
		              value < 0 ? "driver" : "forced by property", quirk.description);
		Log(line);
		if (enabled) {
			g_summary += " ";
			g_summary += quirk.name;
			any = true;
		}
	}
	if (!any) {
		g_summary += " none";
	}
}

const std::string& DriverQuirksSummary() {
	return g_summary;
}

} // namespace Libs::Graphics
