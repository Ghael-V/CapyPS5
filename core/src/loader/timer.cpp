// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/timer.h"

#include "loader/timer.h"

namespace Loader::Timer {

static Common::Timer g_timer;

void Start() {
	g_timer.Start();
}

double GetTimeMs() {
	return g_timer.GetTimeMs();
}

} // namespace Loader::Timer
