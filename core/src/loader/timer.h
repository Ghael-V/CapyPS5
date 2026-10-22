// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

namespace Loader::Timer {

void   Start();
double GetTimeMs();

struct Lifecycle {
	static constexpr const char* name       = "Timer";
	static constexpr auto        initialize = Loader::Timer::Start;
};

} // namespace Loader::Timer

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
