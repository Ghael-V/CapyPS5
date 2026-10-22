// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("WebBrowserDialog", 1, "WebBrowserDialog", 1, 1);

namespace WebBrowserDialog {

constexpr int WEB_BROWSER_DIALOG_STATUS_NONE = 0;

static int KYTY_SYSV_ABI WebBrowserDialogGetStatus() {
	PRINT_NAME();

	return WEB_BROWSER_DIALOG_STATUS_NONE;
}

} // namespace WebBrowserDialog

LIB_DEFINE(InitWebBrowserDialog_1) {
	LIB_FUNC("CFTG6a8TjOU", WebBrowserDialog::WebBrowserDialogGetStatus);
}

} // namespace Libs
