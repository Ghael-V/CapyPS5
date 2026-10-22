// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "libs/errno.h"

namespace Libs::Posix {

static thread_local int g_errno = 0;

KYTY_SYSV_ABI int* GetErrorAddr() {
	return &g_errno;
}

} // namespace Libs::Posix
