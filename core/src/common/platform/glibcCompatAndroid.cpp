// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Compatibility shims for Linux glibc symbols when linking against
// static Linux aarch64 libraries (e.g. FFmpeg) under Android Bionic libc.

#if defined(__ANDROID__)

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

extern "C" {

int* __errno_location(void) {
	return __errno();
}

int __isoc99_sscanf(const char* str, const char* format, ...) {
	va_list args;
	va_start(args, format);
	int ret = vsscanf(str, format, args);
	va_end(args);
	return ret;
}

int fcntl64(int fd, int cmd, ...) {
	va_list args;
	va_start(args, cmd);
	void* arg = va_arg(args, void*);
	va_end(args);
	return fcntl(fd, cmd, arg);
}

int __xpg_strerror_r(int errnum, char* buf, size_t buflen) {
	char* res = strerror_r(errnum, buf, buflen);
	if (res != buf && buf && buflen > 0) {
		strncpy(buf, res, buflen - 1);
		buf[buflen - 1] = '\0';
	}
	return 0;
}

int __fprintf_chk(FILE* stream, int /*flag*/, const char* format, ...) {
	va_list args;
	va_start(args, format);
	int ret = vfprintf(stream, format, args);
	va_end(args);
	return ret;
}

int __printf_chk(int /*flag*/, const char* format, ...) {
	va_list args;
	va_start(args, format);
	int ret = vprintf(format, args);
	va_end(args);
	return ret;
}

int __vfprintf_chk(FILE* stream, int /*flag*/, const char* format, va_list ap) {
	return vfprintf(stream, format, ap);
}

// Stubs for non-PIC FFmpeg NEON audio transforms unused by CapyPS5
void ff_tx_fft16_float_neon() {}
void ff_tx_fft16_ns_float_neon() {}
void ff_tx_fft2_float_neon() {}
void ff_tx_fft32_float_neon() {}
void ff_tx_fft32_ns_float_neon() {}
void ff_tx_fft4_fwd_float_neon() {}
void ff_tx_fft4_inv_float_neon() {}
void ff_tx_fft8_float_neon() {}
void ff_tx_fft8_ns_float_neon() {}
void ff_tx_fft_sr_float_neon() {}
void ff_tx_fft_sr_ns_float_neon() {}

} // extern "C"

#endif // __ANDROID__
