// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static KYTY_SYSV_ABI uint32_t RazorCpuIsCapturing() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
}

} // namespace LibRazorCpu

} // namespace Libs
