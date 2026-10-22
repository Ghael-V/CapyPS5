// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <filesystem>
#include <string>

namespace Kyty::Updater {
struct Result {
	bool        success;
	std::string message;
};

// Only release files are replaced. Files absent from the release are preserved.
// Successful installation consumes staged files; stage on the target filesystem.
Result Validate(const std::filesystem::path& source, const std::filesystem::path& target,
                const std::filesystem::path& launcher_relative);
Result Install(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::filesystem::path& launcher_relative);
} // namespace Kyty::Updater
