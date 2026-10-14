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
