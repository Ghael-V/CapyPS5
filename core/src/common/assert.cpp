// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

#ifdef __ANDROID__
#include <pthread.h>
#endif

namespace Common {

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
#include <android/log.h>
#endif

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Core", "Not implemented: %s in %s:%d (non-fatal, continuing)", expr, file, line);
	return 0;
#else
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
#endif
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
#ifdef __ANDROID__
	// Ending only the failing thread left the game frozen with no sign of the error (a shader
	// decode failure on the GPU thread looked like a hang). Close the app; the log has the report.
	Log::WriteFatal(fmt::format("DbgExit: closing the emulator (status {})", status));
	std::_Exit(status);
#else
	std::_Exit(status);
#endif
}

} // namespace Common
