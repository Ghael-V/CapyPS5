// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_COMMON_CONFIG_H_
#define KYTY_COMMON_CONFIG_H_

#define KYTY_PLATFORM_WINDOWS 1
#define KYTY_PLATFORM_LINUX   5
#ifndef KYTY_PLATFORM_ANDROID
#define KYTY_PLATFORM_ANDROID 6
#endif

#define KYTY_COMPILER_GCC   1
#define KYTY_COMPILER_CLANG 2

#define KYTY_LINKER_LD       1
#define KYTY_LINKER_LLD      2
#define KYTY_LINKER_LLD_LINK 3

#define KYTY_BUILD_DEBUG   1
#define KYTY_BUILD_RELEASE 2

#define KYTY_ENDIAN_BIG    1
#define KYTY_ENDIAN_LITTLE 2

#include "cmake_config.h"

#endif /* KYTY_COMMON_CONFIG_H_ */
