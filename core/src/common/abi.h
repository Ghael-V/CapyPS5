// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_COMMON_ABI_H_
#define KYTY_COMMON_ABI_H_

#include "common/common.h"

#if defined(__x86_64__) || defined(_M_X64)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI __attribute__((ms_abi))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
#else
#define KYTY_MS_ABI
#define KYTY_SYSV_ABI
#endif

#endif /* KYTY_COMMON_ABI_H_ */
