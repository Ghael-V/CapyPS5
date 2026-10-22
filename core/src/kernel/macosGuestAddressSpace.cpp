// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#if defined(__APPLE__) && defined(__x86_64__)

// Make the process own the guest ranges before any runtime initialization.
asm(".zerofill SYSTEM_MANAGED,SYSTEM_MANAGED,__kyty_system_managed,0x7fffbc000");
asm(".zerofill SYSTEM_RESERVED,SYSTEM_RESERVED,__kyty_system_reserved,0x7c0004000");
asm(".zerofill USER_AREA,USER_AREA,__kyty_user_area,0x8c00000000");

#endif
