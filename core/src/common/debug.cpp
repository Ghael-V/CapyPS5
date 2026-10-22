// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/debug.h"

namespace Common {

std::string Debug::GetCompiler() {
#if KYTY_COMPILER == KYTY_COMPILER_CLANG
	return "clang";
#elif KYTY_COMPILER == KYTY_COMPILER_GCC
	return "gcc";
#else
	return "????";
#endif
}

std::string Debug::GetLinker() {
#if KYTY_LINKER == KYTY_LINKER_LD
	return "ld";
#elif KYTY_LINKER == KYTY_LINKER_LLD
	return "lld";
#elif KYTY_LINKER == KYTY_LINKER_LLD_LINK
	return "lld_link";
#else
	return "??";
#endif
}

} // namespace Common
