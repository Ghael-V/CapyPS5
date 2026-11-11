// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef GRAPHICS_HOST_GPU_DRIVER_QUIRKS_H
#define GRAPHICS_HOST_GPU_DRIVER_QUIRKS_H

#include <cstdint>
#include <string>

namespace Libs::Graphics {

// The host Vulkan driver, as VkPhysicalDeviceProperties and VkPhysicalDeviceDriverProperties
// report it.
struct DriverInfo {
	uint32_t    vendor_id      = 0;
	uint32_t    device_id      = 0;
	uint32_t    driver_id      = 0; // VkDriverId
	uint32_t    driver_version = 0;
	std::string device_name;
	std::string driver_name;
	std::string driver_info;
};

// Applies the workarounds the driver needs (the table in driverQuirks.cpp) to
// ShaderRecompiler::g_host_features and the renderer, and logs them (tag CapyPS5-Quirks). Each
// quirk can be forced with its property: `setprop debug.capy.quirk_<name> 0|1` (-1 or unset:
// decided by the driver). Call once at device creation, before any shader is compiled.
void ApplyDriverQuirks(const DriverInfo& info);

// One line with the driver and the quirks in effect, for logs and bug reports.
[[nodiscard]] const std::string& DriverQuirksSummary();

} // namespace Libs::Graphics

#endif // GRAPHICS_HOST_GPU_DRIVER_QUIRKS_H
