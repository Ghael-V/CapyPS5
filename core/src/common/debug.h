// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_COMMON_DEBUG_H_
#define KYTY_COMMON_DEBUG_H_

#include "common/common.h"

#include <string>

namespace Common {

namespace Debug {
std::string GetCompiler();
std::string GetLinker();
} // namespace Debug

} // namespace Common

#endif /* KYTY_COMMON_DEBUG_H_ */
