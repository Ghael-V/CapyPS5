// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "cpu/cpuBackend.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/pthread.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/guestPrintf.h"
#include "libs/libs.h"
#include "libs/vaContext.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <fmt/format.h>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#include <utime.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Libs {

namespace LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
const char* GetProgName();
} // namespace LibKernel

void GuestProcessExit(const char* api, int code) {
	const auto caller = Cpu::GuestReturnAddress();
	auto*      rt     = Common::Singleton<Loader::RuntimeLinker>::Instance();
	const auto* program = caller != 0 ? rt->FindProgramByAddr(caller) : nullptr;
	const auto  message = fmt::format(
	    "Guest {}({}) from 0x{:x} ({} +0x{:x}): the game closed itself", api, code, caller,
	    program != nullptr ? program->file_name.filename().string() : std::string("?"),
	    program != nullptr ? caller - program->base_vaddr : 0);
	Log::WriteFatal(message);
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Core", "%s", message.c_str());
#endif
	std::fflush(nullptr);
	std::_Exit(code);
}

namespace LibC {

LIB_VERSION("libc", 1, "libc", 1, 1);

static uint32_t g_need_flag = 1;

using cxa_destructor_func_t = KYTY_SYSV_ABI void (*)(void*);
using atexit_func_t         = KYTY_SYSV_ABI void (*)();

struct CxaDestructor {
	cxa_destructor_func_t destructor_func;
	void*                 destructor_object;
	void*                 module_id;
};

struct CContext {
	std::list<CxaDestructor> cxa;
	std::list<atexit_func_t> atexit;
};

using InitEnvParams = Loader::EntryParams;

static int                g_argc = 0;
static const char* const* g_argv = nullptr;
static const char* const* g_envp = nullptr;

int GetArgc() {
	return g_argc;
}

const char** GetArgv() {
	return const_cast<const char**>(g_argv);
}

static KYTY_SYSV_ABI void exit(int code) {
	PRINT_NAME();

	GuestProcessExit("libc exit", code);
}

static void PrintAbortStringCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	const auto* ptr = reinterpret_cast<const char*>(static_cast<uintptr_t>(addr));
	char        buf[257] {};
	size_t      len = 0;

	for (; len < sizeof(buf) - 1 && Graphics::HostMemoryIsReadable(addr + len); len++) {
		const auto c = static_cast<unsigned char>(ptr[len]);
		if (c == 0) {
			break;
		}
		if (!std::isprint(c) && c != '\n' && c != '\r' && c != '\t') {
			return;
		}
		buf[len] = static_cast<char>(c);
	}

	if (len >= 4) {
		LOGF("\t %s string = \"%s\"\n", name, buf);
	}
}

static void PrintAbortWideStringCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	const auto* ptr = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(addr));
	char        buf[257] {};
	size_t      len = 0;

	for (;
	     len < sizeof(buf) - 1 && Graphics::HostMemoryIsReadable(addr + len * sizeof(uint16_t) + 1);
	     len++) {
		const auto c = ptr[len];
		if (c == 0) {
			break;
		}
		if (c > 0x7f ||
		    (!std::isprint(static_cast<unsigned char>(c)) && c != '\n' && c != '\r' && c != '\t')) {
			return;
		}
		buf[len] = static_cast<char>(c);
	}

	if (len >= 4) {
		LOGF("\t %s u16 = \"%s\"\n", name, buf);
	}
}

static void PrintAbortBytesCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	uint8_t bytes[64] {};
	size_t  len = 0;

	for (; len < sizeof(bytes) && Graphics::HostMemoryIsReadable(addr + len); len++) {
		bytes[len] = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr + len));
	}

	if (len == 0) {
		return;
	}

	LOGF("\t %s bytes =", name);
	for (size_t i = 0; i < len; i++) {
		LOGF(" %02" PRIx8, bytes[i]);
	}
	LOGF("\n");
}

static void PrintAbortPointerCandidate(const char* name, uint64_t addr) {
	PrintAbortStringCandidate(name, addr);
	PrintAbortWideStringCandidate(name, addr);
	PrintAbortBytesCandidate(name, addr);
}

static bool LooksLikeAbortPointer(uint64_t addr) {
	return Graphics::HostMemoryIsReadable(addr) && addr >= 0x10000;
}

static void PrintAbortPointerArrayCandidate(const char* name, uint64_t addr) {
	if (!LooksLikeAbortPointer(addr)) {
		return;
	}

	for (int i = 0; i < 8; i++) {
		const auto slot_addr = addr + static_cast<uint64_t>(i) * sizeof(uint64_t);
		if (!Graphics::HostMemoryIsReadable(slot_addr)) {
			break;
		}

		const auto value = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(slot_addr));
		if (!LooksLikeAbortPointer(value)) {
			continue;
		}

		LOGF("\t %s[%d] = 0x%016" PRIx64 "\n", name, i, value);
		const auto child_name = fmt::format("{}[{}]", name, i);
		PrintAbortPointerCandidate(child_name.c_str(), value);
	}
}

[[noreturn]] static KYTY_SYSV_ABI void abort(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                             uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	uint64_t rbp = 0;
	uint64_t rsp = 0;
	// This assembly is x86-64-specific, not Windows-specific.
#if defined(__x86_64__) || defined(_M_X64)
	asm volatile("movq %%rbp, %0" : "=r"(rbp));
	asm volatile("movq %%rsp, %0" : "=r"(rsp));
#endif

	const auto ret = reinterpret_cast<uint64_t>(__builtin_return_address(0));

#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Core",
	                    "Guest abort() called! ret=0x%llx rbp=0x%llx rsp=0x%llx a0=0x%llx a1=0x%llx",
	                    static_cast<unsigned long long>(ret), static_cast<unsigned long long>(rbp),
	                    static_cast<unsigned long long>(rsp), static_cast<unsigned long long>(arg0),
	                    static_cast<unsigned long long>(arg1));
#endif

	LOGF("Guest abort diagnostics:\n"
	     "\t return = 0x%016" PRIx64 "\n"
	     "\t rbp    = 0x%016" PRIx64 "\n"
	     "\t rsp    = 0x%016" PRIx64 "\n"
	     "\t arg0   = 0x%016" PRIx64 "\n"
	     "\t arg1   = 0x%016" PRIx64 "\n"
	     "\t arg2   = 0x%016" PRIx64 "\n"
	     "\t arg3   = 0x%016" PRIx64 "\n"
	     "\t arg4   = 0x%016" PRIx64 "\n"
	     "\t arg5   = 0x%016" PRIx64 "\n",
	     ret, rbp, rsp, arg0, arg1, arg2, arg3, arg4, arg5);

	PrintAbortPointerCandidate("arg0", arg0);
	PrintAbortPointerCandidate("arg1", arg1);
	PrintAbortPointerCandidate("arg2", arg2);
	PrintAbortPointerCandidate("arg3", arg3);
	PrintAbortPointerCandidate("arg4", arg4);
	PrintAbortPointerCandidate("arg5", arg5);
	PrintAbortPointerArrayCandidate("arg0", arg0);
	PrintAbortPointerArrayCandidate("arg1", arg1);
	PrintAbortPointerArrayCandidate("arg2", arg2);
	PrintAbortPointerArrayCandidate("arg3", arg3);
	PrintAbortPointerArrayCandidate("arg4", arg4);
	PrintAbortPointerArrayCandidate("arg5", arg5);

	if (rbp != 0) {
		Common::Singleton<Loader::RuntimeLinker>::Instance()->StackTrace(rbp, rsp);
	}

	if (rsp != 0) {
		LOGF("Guest abort stack words:\n");
		auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
		for (int i = 0; i < 16; i++) {
			const auto addr = rsp + static_cast<uint64_t>(i) * sizeof(uint64_t);
			if (!Graphics::HostMemoryIsReadable(addr)) {
				break;
			}
			const auto value   = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(addr));
			auto*      program = linker->FindProgramByAddr(value);
			if (program != nullptr) {
				auto module_name = Common::PathToString(program->file_name.filename());
				LOGF("\t [%02d] 0x%016" PRIx64 " %s+0x%016" PRIx64 "\n", i, value,
				     module_name.c_str(), value - program->base_vaddr);
			} else {
				LOGF("\t [%02d] 0x%016" PRIx64 "\n", i, value);
			}
			PrintAbortPointerCandidate(fmt::format("stack[{:02d}]", i).c_str(), value);
			PrintAbortPointerArrayCandidate(fmt::format("stack[{:02d}]", i).c_str(), value);
		}
	}

	EXIT("Guest abort()\n");
	std::abort();
}

static KYTY_SYSV_ABI int* libc_error() {
	PRINT_NAME();

	return Posix::GetErrorAddr();
}

static KYTY_SYSV_ABI void init_env(const InitEnvParams* params) {
	PRINT_NAME();

	if (params == nullptr) {
		g_argc = 0;
		g_argv = nullptr;
		g_envp = nullptr;
		return;
	}

	EXIT_NOT_IMPLEMENTED(params->argc < 0);

	g_argc = params->argc;
	g_argv = params->argv;
	g_envp = params->argv + params->argc + 1;

	LOGF("\t argc = %d\n"
	     "\t argv = 0x%016" PRIx64 "\n"
	     "\t envp = 0x%016" PRIx64 "\n",
	     g_argc, reinterpret_cast<uint64_t>(g_argv), reinterpret_cast<uint64_t>(g_envp));

	for (int i = 0; i < g_argc; i++) {
		LOGF("\t argv[%d] = %s\n", i, g_argv[i] != nullptr ? g_argv[i] : "<null>");
	}
}

static KYTY_SYSV_ABI int atexit(atexit_func_t func) {
	PRINT_NAME();

	if (func != nullptr) {
		Common::Singleton<CContext>::Instance()->atexit.push_front(func);
	}

	return 0;
}

static KYTY_SYSV_ABI int libc_printf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetGuestPrintfCtxFunc()(&ctx);
}

static KYTY_SYSV_ABI int puts(const char* s) {
	PRINT_NAME();

	return GetGuestPrintfStdFunc()("%s\n", s);
}

static KYTY_SYSV_ABI int setenv(const char* name, const char* value, int overwrite) {
	PRINT_NAME();

	LOGF("\t name      = %s\n"
	     "\t value     = %s\n"
	     "\t overwrite = %d\n",
	     (name != nullptr ? name : "<null>"), (value != nullptr ? value : "<null>"), overwrite);

	if (name == nullptr || value == nullptr || name[0] == '\0' ||
	    std::strchr(name, '=') != nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	// Guest environment changes must not leak into emulator host libraries.
	return 0;
}

static KYTY_SYSV_ABI int64_t libc_time(int64_t* timer) {
	const auto now = static_cast<int64_t>(std::time(nullptr));
	if (timer != nullptr) {
		*timer = now;
	}
	return now;
}

static KYTY_SYSV_ABI double libc_difftime(int64_t time1, int64_t time0) {
	return std::difftime(static_cast<std::time_t>(time1), static_cast<std::time_t>(time0));
}

struct GuestTm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

static_assert(sizeof(GuestTm) == 36);

static GuestTm ToGuestTm(const std::tm& time) {
	return {time.tm_sec,  time.tm_min,  time.tm_hour, time.tm_mday, time.tm_mon,
	        time.tm_year, time.tm_wday, time.tm_yday, time.tm_isdst};
}

static std::tm ToHostTm(const GuestTm& time) {
	std::tm result {};
	result.tm_sec   = time.tm_sec;
	result.tm_min   = time.tm_min;
	result.tm_hour  = time.tm_hour;
	result.tm_mday  = time.tm_mday;
	result.tm_mon   = time.tm_mon;
	result.tm_year  = time.tm_year;
	result.tm_wday  = time.tm_wday;
	result.tm_yday  = time.tm_yday;
	result.tm_isdst = time.tm_isdst;
	return result;
}

static KYTY_SYSV_ABI GuestTm* libc_gmtime(const int64_t* timer) {
	if (timer == nullptr) {
		return nullptr;
	}

	thread_local GuestTm result {};
	std::tm              host_result {};
	const auto           t = static_cast<std::time_t>(*timer);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_gmtime64_s(&host_result, &t) != 0) {
		return nullptr;
	}
#else
	if (gmtime_r(&t, &host_result) == nullptr) {
		return nullptr;
	}
#endif

	result = ToGuestTm(host_result);
	return &result;
}

static KYTY_SYSV_ABI GuestTm* libc_localtime(const int64_t* timer) {
	if (timer == nullptr) {
		return nullptr;
	}

	thread_local GuestTm result {};
	std::tm              host_result {};
	const auto           t = static_cast<std::time_t>(*timer);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_localtime64_s(&host_result, &t) != 0) {
		return nullptr;
	}
#else
	if (localtime_r(&t, &host_result) == nullptr) {
		return nullptr;
	}
#endif

	result = ToGuestTm(host_result);
	return &result;
}

static KYTY_SYSV_ABI int64_t libc_mktime(GuestTm* timeptr) {
	if (timeptr == nullptr) {
		return -1;
	}

	auto host_time = ToHostTm(*timeptr);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto result = static_cast<int64_t>(_mktime64(&host_time));
#else
	const auto result = static_cast<int64_t>(std::mktime(&host_time));
#endif

	*timeptr = ToGuestTm(host_time);
	return result;
}

static KYTY_SYSV_ABI size_t libc_strftime(char* str, size_t count, const char* format,
                                          const GuestTm* timeptr) {
	if (str == nullptr || format == nullptr || timeptr == nullptr) {
		return 0;
	}

	const auto host_time = ToHostTm(*timeptr);
	return std::strftime(str, count, format, &host_time);
}

static KYTY_SYSV_ABI void catchReturnFromMain(int status) {
	PRINT_NAME();

	uint64_t ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	uint64_t rsp      = 0;
	uint64_t rbp      = 0;
#if defined(__x86_64__) || defined(_M_X64)
	asm volatile("movq %%rsp, %0\n\t"
	             "movq %%rbp, %1\n\t"
	             : "=r"(rsp), "=r"(rbp));
#endif

	LOGF("\t status     = %d\n"
	     "\t ret_addr   = 0x%016" PRIx64 "\n"
	     "\t rsp        = 0x%016" PRIx64 "\n"
	     "\t rbp        = 0x%016" PRIx64 "\n",
	     status, ret_addr, rsp, rbp);
	if (rsp != 0) {
		auto* stack = reinterpret_cast<const uint64_t*>(rsp);
		for (uint32_t i = 0; i < 12; i++) {
			LOGF("\t stack[%02" PRIu32 "] = 0x%016" PRIx64 "\n", i, stack[i]);
		}
	}

	::printf("return from main = %d\n", status);
}

using execute_once_func_t = KYTY_SYSV_ABI int (*)(void*, void*, void**);

static std::mutex              g_execute_once_mutex;
static std::condition_variable g_execute_once_cv;

static KYTY_SYSV_ABI int std_execute_once(int* flag, execute_once_func_t func, void* arg) {
	PRINT_NAME();

	if (flag == nullptr || func == nullptr) {
		return 0;
	}

	constexpr int once_init    = 0;
	constexpr int once_done    = 1;
	constexpr int once_running = 2;

	{
		std::unique_lock lock(g_execute_once_mutex);
		while (*flag == once_running) {
			g_execute_once_cv.wait_for(lock, std::chrono::microseconds(10000));
			if (*flag == once_running) {
				lock.unlock();
				LibKernel::KernelDispatchPendingSignalForCurrentThread();
				lock.lock();
			}
		}
		if (*flag == once_done) {
			return 1;
		}
		*flag = once_running;
	}

	void* callback_context = nullptr;
	int   result           = static_cast<int>(
	    Cpu::CpuBackend::Instance()->CallGuestCallback(reinterpret_cast<uint64_t>(func),
	                                                   0,
	                                                   reinterpret_cast<uint64_t>(arg),
	                                                   reinterpret_cast<uint64_t>(&callback_context)));

	{
		std::lock_guard lock(g_execute_once_mutex);
		*flag = (result != 0 ? once_done : once_init);
	}
	g_execute_once_cv.notify_all();

	return result;
}

static KYTY_SYSV_ABI int cxa_atexit(cxa_destructor_func_t func, void* arg, void* d) {
	PRINT_NAME();

	auto* cc = Common::Singleton<CContext>::Instance();

	CxaDestructor c {};
	c.destructor_func   = func;
	c.destructor_object = arg;
	c.module_id         = d;

	cc->cxa.push_back(c);

	return 0;
}

void KYTY_SYSV_ABI cxa_finalize(void* d) {
	PRINT_NAME();

	auto* cc = Common::Singleton<CContext>::Instance();

	for (auto i = cc->cxa.rbegin(); i != cc->cxa.rend(); ++i) {
		auto& c = *i;
		if ((d == nullptr || c.module_id == d) && c.destructor_func != nullptr) {
			auto* func        = c.destructor_func;
			auto* object      = c.destructor_object;
			c.destructor_func = nullptr;
			Cpu::CpuBackend::Instance()->CallGuestCallback(reinterpret_cast<uint64_t>(func),
			                                               reinterpret_cast<uint64_t>(object));
		}
	}
}

} // namespace LibC

namespace LibcInternalExt {

LIB_VERSION("LibcInternalExt", 1, "LibcInternal", 1, 1);

static uint64_t g_mspace_atomic_id_mask = 0;
static uint64_t g_mstate_table[64]      = {0};

using thread_atexit_destructor_t = KYTY_SYSV_ABI void (*)(void*);

struct ThreadAtexitDestructor {
	thread_atexit_destructor_t destructor;
	void*                      object;
};

static thread_local std::vector<ThreadAtexitDestructor> g_thread_atexit_destructors;

struct Info {
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};

void KYTY_SYSV_ABI LibcHeapGetTraceInfo(Info* info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(info->size != 32);

	info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
	info->mstate_table          = g_mstate_table;
}

int KYTY_SYSV_ABI LibcInternalExtCxaThreadAtexit(thread_atexit_destructor_t destructor,
                                                 void* object, void* /*module_id*/) {
	PRINT_NAME();

	g_thread_atexit_destructors.push_back({destructor, object});

	return 0;
}

void RunThreadAtexitDestructors() {
	while (!g_thread_atexit_destructors.empty()) {
		auto destructor = g_thread_atexit_destructors.back();
		g_thread_atexit_destructors.pop_back();

		if (destructor.destructor != nullptr) {
			Cpu::CpuBackend::Instance()->CallGuestCallback(reinterpret_cast<uint64_t>(destructor.destructor),
			                                               reinterpret_cast<uint64_t>(destructor.object));
		}
	}
}

LIB_DEFINE(InitLibcInternalExt_1) {
	LIB_FUNC("NWtTN10cJzE", LibcInternalExt::LibcHeapGetTraceInfo);
	LIB_FUNC("qBS714-Jr3g", LibcInternalExt::LibcInternalExtCxaThreadAtexit);
}

} // namespace LibcInternalExt

namespace LibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static uint32_t g_need_flag = 1;

int KYTY_SYSV_ABI vprintf(const char* str, VaList* c) {
	PRINT_NAME();

	return GetGuestVprintfFunc()(str, c);
}

static KYTY_SYSV_ABI int snprintf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetGuestSnprintfCtxFunc()(&ctx);
}

int KYTY_SYSV_ABI fflush(FILE* stream) {
	PRINT_NAME();

	if (stream == nullptr || stream == stdout || stream == stderr) {
		return ::fflush(stream);
	}

	return 0;
}

void* KYTY_SYSV_ABI memset(void* s, int c, size_t n) {
	PRINT_NAME();

	return ::memset(s, c, n);
}

void* KYTY_SYSV_ABI memcpy(void* dest, const void* src, size_t n) {
	return ::memcpy(dest, src, n);
}

void* KYTY_SYSV_ABI memmove(void* dest, const void* src, size_t n) {
	return ::memmove(dest, src, n);
}

int KYTY_SYSV_ABI memcmp(const void* s1, const void* s2, size_t n) {
	return ::memcmp(s1, s2, n);
}

int KYTY_SYSV_ABI strcmp(const char* s1, const char* s2) {
	return ::strcmp(s1, s2);
}

int KYTY_SYSV_ABI strncmp(const char* s1, const char* s2, size_t n) {
	return ::strncmp(s1, s2, n);
}

size_t KYTY_SYSV_ABI strlen(const char* s) {
	return ::strlen(s);
}

char* KYTY_SYSV_ABI strcpy(char* dest, const char* src) {
	return ::strcpy(dest, src);
}

char* KYTY_SYSV_ABI strncpy(char* dest, const char* src, size_t count) {
	return ::strncpy(dest, src, count);
}

char* KYTY_SYSV_ABI strcat(char* dest, const char* src) {
	return ::strcat(dest, src);
}

const char* KYTY_SYSV_ABI strchr(const char* s, int c) {
	return ::strchr(s, c);
}

char* KYTY_SYSV_ABI strrchr(const char* s, int c) {
	return const_cast<char*>(::strrchr(s, c));
}

char* KYTY_SYSV_ABI strstr(const char* haystack, const char* needle) {
	static int log_count = 0;
	if (log_count++ < 8) {
		LOGF("LibcInternal::strstr(\"%s\", \"%s\")\n", (haystack != nullptr ? haystack : "<null>"),
		     (needle != nullptr ? needle : "<null>"));
	}
	return const_cast<char*>(::strstr(haystack, needle));
}

long KYTY_SYSV_ABI strtol(const char* str, char** endptr, int base) {
	static int log_count = 0;
	if (log_count++ < 16) {
		LOGF("LibcInternal::strtol(\"%s\", base=%d)\n", (str != nullptr ? str : "<null>"), base);
	}
	return ::strtol(str, endptr, base);
}

unsigned long KYTY_SYSV_ABI strtoul(const char* str, char** endptr, int base) {
	static int log_count = 0;
	if (log_count++ < 16) {
		LOGF("LibcInternal::strtoul(\"%s\", base=%d)\n", (str != nullptr ? str : "<null>"), base);
	}
	return ::strtoul(str, endptr, base);
}

int KYTY_SYSV_ABI atoi(const char* str) {
	return ::atoi(str);
}

float KYTY_SYSV_ABI sinf(float x) {
	return std::sin(x);
}

float KYTY_SYSV_ABI cosf(float x) {
	return std::cos(x);
}

void KYTY_SYSV_ABI sincosf(float x, float* sinp, float* cosp) {
	if (sinp != nullptr) {
		*sinp = std::sin(x);
	}
	if (cosp != nullptr) {
		*cosp = std::cos(x);
	}
}

double KYTY_SYSV_ABI sin(double x) {
	return std::sin(x);
}

double KYTY_SYSV_ABI cos(double x) {
	return std::cos(x);
}

void KYTY_SYSV_ABI sincos(double x, double* sinp, double* cosp) {
	if (sinp != nullptr) {
		*sinp = std::sin(x);
	}
	if (cosp != nullptr) {
		*cosp = std::cos(x);
	}
}

int KYTY_SYSV_ABI LibcHeapErrorReportForGame(uint64_t msp, uint64_t ptr, uint64_t error,
                                             uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	LOGF("\t temporary: heap error report ignored, msp=0x%016" PRIx64 ", ptr=0x%016" PRIx64
	     ", error=0x%016" PRIx64 ", args=(0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ")\n",
	     msp, ptr, error, arg3, arg4, arg5);

	return 0;
}

static FILE* g_stdout = stdout;
static FILE* g_stderr = stderr;
static FILE* g_stdin  = stdin;
static int   g_isthreaded = 1;
static int   g_mb_cur_max = 1;

// Guest allocations that fail are logged (a game that does not check them crashes later on a
// null pointer far from the cause).
static void* NoteAllocation(void* result, const char* name, size_t num, size_t size) {
#if defined(__ANDROID__)
	if (result == nullptr && num * size != 0) {
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1) < 32) {
			__android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Alloc", "%s(%zu, %zu) returned null", name, num, size);
		}
	}
#endif
	return result;
}

void* KYTY_SYSV_ABI malloc(size_t size) {
	return NoteAllocation(std::malloc(size), "malloc", 1, size);
}

void KYTY_SYSV_ABI free(void* ptr) {
	std::free(ptr);
}

void* KYTY_SYSV_ABI calloc(size_t num, size_t size) {
	void* result = std::calloc(num, size);
#if defined(__ANDROID__)
	static Common::DebugFlags::CachedValue alloc_log("debug.capy.alloc_log", 0);
	if (alloc_log.Get() != 0 && num * size >= static_cast<size_t>(alloc_log.Get()) && num * size < static_cast<size_t>(alloc_log.Get()) * 2) {
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Alloc", "calloc(%zu, %zu) -> %p", num, size, result);
	}
#endif
	return NoteAllocation(result, "calloc", num, size);
}

void* KYTY_SYSV_ABI realloc(void* ptr, size_t size) {
	return NoteAllocation(std::realloc(ptr, size), "realloc", 1, size);
}

int KYTY_SYSV_ABI posix_memalign(void** memptr, size_t alignment, size_t size) {
	return ::posix_memalign(memptr, alignment, size);
}

size_t KYTY_SYSV_ABI malloc_usable_size(void* ptr) {
#if defined(__ANDROID__) || defined(__linux__)
	return ::malloc_usable_size(ptr);
#else
	return 0;
#endif
}

float KYTY_SYSV_ABI ldexpf(float x, int exp) {
	return std::ldexp(x, exp);
}

double KYTY_SYSV_ABI ldexp(double x, int exp) {
	return std::ldexp(x, exp);
}

double KYTY_SYSV_ABI pow(double x, double y) {
	return std::pow(x, y);
}

float KYTY_SYSV_ABI powf(float x, float y) {
	return std::pow(x, y);
}

float KYTY_SYSV_ABI acosf(float x) {
	return std::acos(x);
}

float KYTY_SYSV_ABI atan2f(float y, float x) {
	return std::atan2(y, x);
}

float KYTY_SYSV_ABI tanf(float x) {
	return std::tan(x);
}

float KYTY_SYSV_ABI fmodf(float x, float y) {
	return std::fmod(x, y);
}

float KYTY_SYSV_ABI expf(float x) {
	return std::exp(x);
}

float KYTY_SYSV_ABI exp2f(float x) {
	return std::exp2(x);
}

double KYTY_SYSV_ABI exp(double x) {
	return std::exp(x);
}

double KYTY_SYSV_ABI exp2(double x) {
	return std::exp2(x);
}

double KYTY_SYSV_ABI log(double x) {
	return std::log(x);
}

double KYTY_SYSV_ABI log2(double x) {
	return std::log2(x);
}

float KYTY_SYSV_ABI log2f(float x) {
	return std::log2(x);
}

double KYTY_SYSV_ABI log10(double x) {
	return std::log10(x);
}

float KYTY_SYSV_ABI log10f(float x) {
	return std::log10(x);
}

double KYTY_SYSV_ABI tanh(double x) {
	return std::tanh(x);
}

float KYTY_SYSV_ABI tanhf(float x) {
	return std::tanh(x);
}

float KYTY_SYSV_ABI hypotf(float x, float y) {
	return std::hypot(x, y);
}

long KYTY_SYSV_ABI lroundf(float x) {
	return std::lround(x);
}

long long KYTY_SYSV_ABI llround(double x) {
	return std::llround(x);
}

float KYTY_SYSV_ABI frexpf(float x, int* exp) {
	return std::frexp(x, exp);
}

double KYTY_SYSV_ABI frexp(double x, int* exp) {
	return std::frexp(x, exp);
}

float KYTY_SYSV_ABI remainderf(float x, float y) {
	return std::remainder(x, y);
}

int KYTY_SYSV_ABI isprint(int c) {
	return std::isprint(c);
}

int KYTY_SYSV_ABI isspace(int c) {
	return std::isspace(c);
}

int KYTY_SYSV_ABI isalnum(int c) {
	return std::isalnum(c);
}

int KYTY_SYSV_ABI isxdigit(int c) {
	return std::isxdigit(c);
}

int KYTY_SYSV_ABI toupper(int c) {
	return std::toupper(c);
}

int KYTY_SYSV_ABI tolower(int c) {
	return std::tolower(c);
}

char* KYTY_SYSV_ABI strtok(char* str, const char* delim) {
	return ::strtok(str, delim);
}

double KYTY_SYSV_ABI strtod(const char* str, char** endptr) {
	return ::strtod(str, endptr);
}

float KYTY_SYSV_ABI strtof(const char* str, char** endptr) {
	return ::strtof(str, endptr);
}

long long KYTY_SYSV_ABI strtoll(const char* str, char** endptr, int base) {
	return ::strtoll(str, endptr, base);
}

unsigned long long KYTY_SYSV_ABI strtoull(const char* str, char** endptr, int base) {
	return ::strtoull(str, endptr, base);
}

double KYTY_SYSV_ABI atof(const char* str) {
	return ::atof(str);
}

char* KYTY_SYSV_ABI strdup(const char* s) {
	return ::strdup(s);
}

char* KYTY_SYSV_ABI strndup(const char* s, size_t n) {
	return ::strndup(s, n);
}

size_t KYTY_SYSV_ABI strnlen(const char* s, size_t maxlen) {
	return ::strnlen(s, maxlen);
}

int KYTY_SYSV_ABI strcasecmp(const char* s1, const char* s2) {
	return ::strcasecmp(s1, s2);
}

char* KYTY_SYSV_ABI strncat(char* dest, const char* src, size_t n) {
	return ::strncat(dest, src, n);
}

char* KYTY_SYSV_ABI strpbrk(const char* s, const char* accept) {
	return const_cast<char*>(::strpbrk(s, accept));
}

size_t KYTY_SYSV_ABI strcspn(const char* s, const char* reject) {
	return ::strcspn(s, reject);
}

size_t KYTY_SYSV_ABI strspn(const char* s, const char* accept) {
	return ::strspn(s, accept);
}

char* KYTY_SYSV_ABI strerror(int errnum) {
	return ::strerror(errnum);
}

int KYTY_SYSV_ABI strerror_r(int errnum, char* buf, size_t buflen) {
	if (!buf || buflen == 0) return -1;
#if defined(__ANDROID__) || (defined(_GNU_SOURCE) && !defined(__APPLE__))
	char* res = ::strerror_r(errnum, buf, buflen);
	if (res != buf && res != nullptr) {
		std::strncpy(buf, res, buflen - 1);
		buf[buflen - 1] = '\0';
	}
	return 0;
#else
	return ::strerror_r(errnum, buf, buflen);
#endif
}

void* KYTY_SYSV_ABI memchr(const void* s, int c, size_t n) {
	return const_cast<void*>(::memchr(s, c, n));
}

char* KYTY_SYSV_ABI ctime(const time_t* timep) {
	return ::ctime(timep);
}

FILE* KYTY_SYSV_ABI fopen(const char* pathname, const char* mode) {
	if (pathname == nullptr) {
		return nullptr;
	}
	auto real_path = LibKernel::FileSystem::GetRealFilename(pathname);
	if (real_path.empty() && pathname[0] != '/') {
		real_path = LibKernel::FileSystem::GetRealFilename(std::string("/") + pathname);
	}
	std::string target_str;
	const char* target = nullptr;
	if (!real_path.empty()) {
		target_str = real_path.string();
		target = target_str.c_str();
		if (mode && (std::strchr(mode, 'w') != nullptr || std::strchr(mode, 'a') != nullptr)) {
			std::error_code ec;
			std::filesystem::create_directories(real_path.parent_path(), ec);
		}
	} else {
		target = pathname;
	}
	FILE* f = ::fopen(target, mode);
#if defined(__ANDROID__)
	if (Common::DebugFlags::Verbose())
	__android_log_print(ANDROID_LOG_INFO, "CapyPS5-LibC", "fopen: guest='%s' -> host='%s' (mode='%s') -> %p",
	                    pathname, target, mode ? mode : "", f);
#endif
	return f;
}

int KYTY_SYSV_ABI fclose(FILE* stream) {
	if (stream == nullptr) return 0;
	return ::fclose(stream);
}

size_t KYTY_SYSV_ABI fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
	if (stream == nullptr || ptr == nullptr) return 0;
	return ::fread(ptr, size, nmemb, stream);
}

size_t KYTY_SYSV_ABI fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
	if (stream == nullptr || ptr == nullptr) return 0;
	return ::fwrite(ptr, size, nmemb, stream);
}

int KYTY_SYSV_ABI fseek(FILE* stream, long offset, int whence) {
	if (stream == nullptr) {
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-LibC", "fseek: stream is NULL! offset=%ld whence=%d", offset, whence);
#endif
		return -1;
	}
	int ret = ::fseeko(stream, static_cast<off_t>(offset), whence);
	if (ret != 0) {
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-LibC", "fseek: stream=%p offset=%ld whence=%d -> error %d: %s",
		                    stream, offset, whence, errno, strerror(errno));
#endif
	}
	return ret;
}

int KYTY_SYSV_ABI fseeko(FILE* stream, off_t offset, int whence) {
	if (stream == nullptr) {
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-LibC", "fseeko: stream is NULL! offset=%" PRId64 " whence=%d", static_cast<int64_t>(offset), whence);
#endif
		return -1;
	}
	int ret = ::fseeko(stream, offset, whence);
	if (ret != 0) {
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-LibC", "fseeko: stream=%p offset=%" PRId64 " whence=%d -> error %d: %s",
		                    stream, static_cast<int64_t>(offset), whence, errno, strerror(errno));
#endif
	}
	return ret;
}

long KYTY_SYSV_ABI ftell(FILE* stream) {
	if (stream == nullptr) return -1;
	return ::ftell(stream);
}

off_t KYTY_SYSV_ABI ftello(FILE* stream) {
	if (stream == nullptr) return -1;
	return ::ftello(stream);
}

int KYTY_SYSV_ABI fileno(FILE* stream) {
	if (stream == nullptr) return -1;
	return ::fileno(stream);
}

int KYTY_SYSV_ABI feof(FILE* stream) {
	if (stream == nullptr) return 1;
	return ::feof(stream);
}

int KYTY_SYSV_ABI ferror(FILE* stream) {
	if (stream == nullptr) return 1;
	return ::ferror(stream);
}

int KYTY_SYSV_ABI fputs(const char* s, FILE* stream) {
	return ::fputs(s, stream);
}

int KYTY_SYSV_ABI fputc(int c, FILE* stream) {
	return ::fputc(c, stream);
}

int KYTY_SYSV_ABI fgetc(FILE* stream) {
	return ::fgetc(stream);
}

char* KYTY_SYSV_ABI fgets(char* s, int size, FILE* stream) {
	return ::fgets(s, size, stream);
}

int KYTY_SYSV_ABI ungetc(int c, FILE* stream) {
	return ::ungetc(c, stream);
}

int KYTY_SYSV_ABI setvbuf(FILE* stream, char* buf, int mode, size_t size) {
	return ::setvbuf(stream, buf, mode, size);
}

void KYTY_SYSV_ABI rewind(FILE* stream) {
	::rewind(stream);
}

int KYTY_SYSV_ABI putchar(int c) {
	return ::putchar(c);
}

int KYTY_SYSV_ABI getc(FILE* stream) {
	return ::getc(stream);
}

int KYTY_SYSV_ABI vsnprintf(char* s, size_t n, const char* format, VaList* c) {
	return GetGuestVsnprintfFunc()(s, n, format, c);
}

static KYTY_SYSV_ABI int sprintf(VA_ARGS) {
	VA_CONTEXT(ctx);
	char*       s      = VaArg_ptr<char>(&ctx.va_list);
	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return GetGuestVsnprintfFunc()(s, static_cast<size_t>(-1), format, &ctx.va_list);
}

int KYTY_SYSV_ABI vfprintf(FILE* stream, const char* format, VaList* c) {
	char buf[1024];
	int len = GetGuestVsnprintfFunc()(buf, sizeof(buf), format, c);
	if (len > 0) {
#if defined(__ANDROID__)
		if (stream == stderr) {
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GuestStderr", "%.*s", len, buf);
		} else {
			__android_log_print(ANDROID_LOG_INFO, "CapyPS5-GuestStdout", "%.*s", len, buf);
		}
#endif
		if (stream != nullptr) {
			std::fwrite(buf, 1, len, stream);
		}
	}
	return len;
}

static KYTY_SYSV_ABI int fprintf(VA_ARGS) {
	VA_CONTEXT(ctx);
	FILE*       stream = VaArg_ptr<FILE>(&ctx.va_list);
	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return vfprintf(stream, format, &ctx.va_list);
}

char* KYTY_SYSV_ABI getenv(const char* name) {
	return ::getenv(name);
}

int KYTY_SYSV_ABI unsetenv(const char* name) {
	return ::unsetenv(name);
}

void KYTY_SYSV_ABI qsort(void* base, size_t nmemb, size_t size, uint64_t compar) {
	if (!base || nmemb <= 1 || size == 0 || compar == 0) {
		return;
	}
	auto* backend = Cpu::CpuBackend::Instance();
	if (!backend) {
		return;
	}

	char* base_bytes = static_cast<char*>(base);
	std::vector<size_t> indices(nmemb);
	for (size_t i = 0; i < nmemb; ++i) {
		indices[i] = i;
	}

	std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
		const void* ptr_a = base_bytes + a * size;
		const void* ptr_b = base_bytes + b * size;
		int res = static_cast<int>(backend->CallGuestCallback(compar,
		                           reinterpret_cast<uint64_t>(ptr_a),
		                           reinterpret_cast<uint64_t>(ptr_b)));
		return res < 0;
	});

	std::vector<char> sorted_copy(nmemb * size);
	for (size_t i = 0; i < nmemb; ++i) {
		std::memcpy(sorted_copy.data() + i * size, base_bytes + indices[i] * size, size);
	}
	std::memcpy(base_bytes, sorted_copy.data(), nmemb * size);
}

int KYTY_SYSV_ABI rand() {
	return ::rand();
}

void* KYTY_SYSV_ABI bsearch(const void* key, const void* base, size_t nmemb, size_t size, uint64_t compar) {
	if (!key || !base || nmemb == 0 || size == 0 || compar == 0) {
		return nullptr;
	}
	auto* backend = Cpu::CpuBackend::Instance();
	if (!backend) return nullptr;

	const char* base_bytes = static_cast<const char*>(base);
	size_t low = 0;
	size_t high = nmemb;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		const void* mid_ptr = base_bytes + mid * size;
		int cmp = static_cast<int>(backend->CallGuestCallback(compar,
		                           reinterpret_cast<uint64_t>(key),
		                           reinterpret_cast<uint64_t>(mid_ptr)));
		if (cmp == 0) {
			return const_cast<void*>(mid_ptr);
		} else if (cmp < 0) {
			high = mid;
		} else {
			low = mid + 1;
		}
	}
	return nullptr;
}

int KYTY_SYSV_ABI remove(const char* pathname) {
	if (pathname == nullptr) return -1;
	auto real_path = LibKernel::FileSystem::GetRealFilename(pathname);
	if (real_path.empty() && pathname[0] != '/') {
		real_path = LibKernel::FileSystem::GetRealFilename(std::string("/") + pathname);
	}
	const char* target = (!real_path.empty()) ? real_path.c_str() : pathname;
	return ::remove(target);
}

const char* KYTY_SYSV_ABI getprogname() {
	return Libs::LibKernel::GetProgName();
}

char* KYTY_SYSV_ABI setlocale(int category, const char* locale) {
	return ::setlocale(category, locale);
}

int KYTY_SYSV_ABI utime(const char* filename, const struct utimbuf* times) {
	if (filename == nullptr) return -1;
	auto real_path = LibKernel::FileSystem::GetRealFilename(filename);
	if (real_path.empty() && filename[0] != '/') {
		real_path = LibKernel::FileSystem::GetRealFilename(std::string("/") + filename);
	}
	const char* target = (!real_path.empty()) ? real_path.c_str() : filename;
#if defined(__ANDROID__) || defined(__linux__)
	return ::utime(target, times);
#else
	return 0;
#endif
}

using ps5_wchar_t = uint16_t;

size_t KYTY_SYSV_ABI wcslen(const ps5_wchar_t* s) {
	if (!s) return 0;
	size_t len = 0;
	while (s[len] != 0) ++len;
	return len;
}

int KYTY_SYSV_ABI iswprint(wint_t wc) {
	return std::iswprint(wc);
}

int KYTY_SYSV_ABI iswspace(wint_t wc) {
	return std::iswspace(wc);
}

int KYTY_SYSV_ABI iswalnum(wint_t wc) {
	return std::iswalnum(wc);
}

int KYTY_SYSV_ABI iswalpha(wint_t wc) {
	return std::iswalpha(wc);
}

int KYTY_SYSV_ABI iswdigit(wint_t wc) {
	return std::iswdigit(wc);
}

int KYTY_SYSV_ABI iswxdigit(wint_t wc) {
	return std::iswxdigit(wc);
}

int KYTY_SYSV_ABI iswpunct(wint_t wc) {
	return std::iswpunct(wc);
}

int KYTY_SYSV_ABI iswupper(wint_t wc) {
	return std::iswupper(wc);
}

int KYTY_SYSV_ABI iswlower(wint_t wc) {
	return std::iswlower(wc);
}

int KYTY_SYSV_ABI iswcntrl(wint_t wc) {
	return std::iswcntrl(wc);
}

int KYTY_SYSV_ABI iswblank(wint_t wc) {
	return std::iswblank(wc);
}

int KYTY_SYSV_ABI iswgraph(wint_t wc) {
	return std::iswgraph(wc);
}

wint_t KYTY_SYSV_ABI towupper(wint_t wc) {
	return std::towupper(wc);
}

wint_t KYTY_SYSV_ABI towlower(wint_t wc) {
	return std::towlower(wc);
}

ps5_wchar_t* KYTY_SYSV_ABI wmemchr(const ps5_wchar_t* s, ps5_wchar_t c, size_t n) {
	if (!s) return nullptr;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == c) return const_cast<ps5_wchar_t*>(s + i);
	}
	return nullptr;
}

int KYTY_SYSV_ABI wmemcmp(const ps5_wchar_t* s1, const ps5_wchar_t* s2, size_t n) {
	if (!s1 || !s2) return 0;
	for (size_t i = 0; i < n; i++) {
		if (s1[i] != s2[i]) return static_cast<int>(s1[i]) - static_cast<int>(s2[i]);
	}
	return 0;
}

int KYTY_SYSV_ABI mbtowc(ps5_wchar_t* pwc, const char* s, size_t n) {
	if (s == nullptr) {
		return 0; // State-independent encoding
	}
	if (n == 0) {
		return -1;
	}
	const auto* u = reinterpret_cast<const uint8_t*>(s);
	uint8_t c0 = u[0];
	if (c0 == 0) {
		if (pwc != nullptr) *pwc = 0;
		return 0;
	}
	if ((c0 & 0x80) == 0) {
		// 1-byte ASCII
		if (pwc != nullptr) *pwc = c0;
		return 1;
	}
	if ((c0 & 0xE0) == 0xC0) {
		// 2-byte UTF-8
		if (n < 2) return -1;
		uint8_t c1 = u[1];
		if ((c1 & 0xC0) != 0x80) return -1;
		uint16_t code = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
		if (pwc != nullptr) *pwc = code;
		return 2;
	}
	if ((c0 & 0xF0) == 0xE0) {
		// 3-byte UTF-8
		if (n < 3) return -1;
		uint8_t c1 = u[1];
		uint8_t c2 = u[2];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return -1;
		uint16_t code = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
		if (pwc != nullptr) *pwc = code;
		return 3;
	}
	if ((c0 & 0xF8) == 0xF0) {
		// 4-byte UTF-8
		if (n < 4) return -1;
		uint8_t c1 = u[1];
		uint8_t c2 = u[2];
		uint8_t c3 = u[3];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return -1;
		uint32_t code = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
		if (pwc != nullptr) {
			if (code <= 0xFFFF) {
				*pwc = static_cast<uint16_t>(code);
			} else {
				*pwc = static_cast<uint16_t>(0xD800 + ((code - 0x10000) >> 10));
			}
		}
		return 4;
	}
	return -1;
}

size_t KYTY_SYSV_ABI mbrtowc(ps5_wchar_t* pwc, const char* s, size_t n, void* /*ps*/) {
	if (s == nullptr) {
		return 0;
	}
	int res = mbtowc(pwc, s, n);
	if (res < 0) return static_cast<size_t>(-1);
	return static_cast<size_t>(res);
}

size_t KYTY_SYSV_ABI wcrtomb(char* s, ps5_wchar_t wc, void* /*ps*/) {
	if (s == nullptr) return 1;
	if (wc < 0x80) {
		s[0] = static_cast<char>(wc);
		return 1;
	} else if (wc < 0x800) {
		s[0] = static_cast<char>(0xC0 | (wc >> 6));
		s[1] = static_cast<char>(0x80 | (wc & 0x3F));
		return 2;
	} else {
		s[0] = static_cast<char>(0xE0 | (wc >> 12));
		s[1] = static_cast<char>(0x80 | ((wc >> 6) & 0x3F));
		s[2] = static_cast<char>(0x80 | (wc & 0x3F));
		return 3;
	}
}

wint_t KYTY_SYSV_ABI btowc(int c) {
	if (c == EOF) return WEOF;
	if ((c & ~0x7F) == 0) return static_cast<wint_t>(c);
	return WEOF;
}

int KYTY_SYSV_ABI wctob(wint_t c) {
	if (c == WEOF || c > 0x7F) return EOF;
	return static_cast<int>(c);
}

wctype_t KYTY_SYSV_ABI wctype(const char* property) {
	return ::wctype(property);
}

int KYTY_SYSV_ABI iswctype(wint_t wc, wctype_t desc) {
	return ::iswctype(wc, desc);
}

LIB_DEFINE(InitLibcInternal_1) {
	LibcInternalExt::InitLibcInternalExt_1(s);

	LIB_OBJECT("ZT4ODD2Ts9o", &LibcInternal::g_need_flag);
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("GMpvxPFW924", LibcInternal::vprintf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("8zTFvBIAIN8", LibcInternal::memset);
	LIB_FUNC("Q3VBxCXhUHs", LibcInternal::memcpy);
	LIB_FUNC("+P6FRGH4LfA", LibcInternal::memmove);
	LIB_FUNC("DfivPArhucg", LibcInternal::memcmp);
	LIB_FUNC("aesyjrHVWy4", LibcInternal::strcmp);
	LIB_FUNC("Ovb2dSJOAuE", LibcInternal::strncmp);
	LIB_FUNC("j4ViWNHEgww", LibcInternal::strlen);
	LIB_FUNC("5Xa2ACNECdo", LibcInternal::strcpy);
	LIB_FUNC("6sJWiWSRuqk", LibcInternal::strncpy);
	LIB_FUNC("Ls4tzzhimqQ", LibcInternal::strcat);
	LIB_FUNC("ob5xAW4ln-0", LibcInternal::strchr);
	LIB_FUNC("9yDWMxEFdJU", LibcInternal::strrchr);
	LIB_FUNC("viiwFMaNamA", LibcInternal::strstr);
	LIB_FUNC("mXlxhmLNMPg", LibcInternal::strtol);
	LIB_FUNC("QxmSHBCuKTk", LibcInternal::strtoul);
	LIB_FUNC("zlfEH8FmyUA", LibcInternal::strtoul);
	LIB_FUNC("fPxypibz2MY", LibcInternal::atoi);
	LIB_FUNC("Q4rRL34CEeE", LibcInternal::sinf);
	LIB_FUNC("-P6FNMzk2Kc", LibcInternal::cosf);
	LIB_FUNC("pztV4AF18iI", LibcInternal::sincosf);
	LIB_FUNC("H8ya2H00jbI", LibcInternal::sin);
	LIB_FUNC("2WE3BTYVwKM", LibcInternal::cos);
	LIB_FUNC("jMB7EFyu30Y", LibcInternal::sincos);
	LIB_FUNC("eLdDw6l0-bU", LibcInternal::snprintf);

	// Memory
	LIB_FUNC("gQX+4GDQjpM", LibcInternal::malloc);
	LIB_FUNC("tIhsqj0qsFE", LibcInternal::free);
	LIB_FUNC("2X5agFjKxMc", LibcInternal::calloc);
	LIB_FUNC("Y7aJ1uydPMo", LibcInternal::realloc);
	LIB_FUNC("cVSk9y8URbc", LibcInternal::posix_memalign);
	LIB_FUNC("NDcSfcYZRC8", LibcInternal::malloc_usable_size);

	// Math
	LIB_FUNC("kn0yiYeExgA", LibcInternal::ldexpf);
	LIB_FUNC("JrwFIMzKNr0", LibcInternal::ldexp);
	LIB_FUNC("9LCjpWyQ5Zc", LibcInternal::pow);
	LIB_FUNC("1D0H2KNjshE", LibcInternal::powf);
	LIB_FUNC("QI-x0SL8jhw", LibcInternal::acosf);
	LIB_FUNC("EH-x713A99c", LibcInternal::atan2f);
	LIB_FUNC("ZE6RNL+eLbk", LibcInternal::tanf);
	LIB_FUNC("88Vv-AzHVj8", LibcInternal::fmodf);
	LIB_FUNC("8zsu04XNsZ4", LibcInternal::expf);
	LIB_FUNC("wuAQt-j+p4o", LibcInternal::exp2f);
	LIB_FUNC("NVadfnzQhHQ", LibcInternal::exp);
	LIB_FUNC("dnaeGXbjP6E", LibcInternal::exp2);
	LIB_FUNC("rtV7-jWC6Yg", LibcInternal::log);
	LIB_FUNC("Y5DhuDKGlnQ", LibcInternal::log2);
	LIB_FUNC("hsi9drzHR2k", LibcInternal::log2f);
	LIB_FUNC("WuMbPBKN1TU", LibcInternal::log10);
	LIB_FUNC("lhpd6Wk6ccs", LibcInternal::log10f);
	LIB_FUNC("JM4EBvWT9rc", LibcInternal::tanh);
	LIB_FUNC("SAd0Z3wKwLA", LibcInternal::tanhf);
	LIB_FUNC("iz2shAGFIxc", LibcInternal::hypotf);
	LIB_FUNC("C6gWCWJKM+U", LibcInternal::lroundf);
	LIB_FUNC("w-BvXF4O6xo", LibcInternal::llround);
	LIB_FUNC("aaDMGGkXFxo", LibcInternal::frexpf);
	LIB_FUNC("kA-TdiOCsaY", LibcInternal::frexp);
	LIB_FUNC("eS+MVq+Lltw", LibcInternal::remainderf);

	// Character / String
	LIB_FUNC("eGkOpTojJl4", LibcInternal::isprint);
	LIB_FUNC("wazw2x2m3DQ", LibcInternal::isspace);
	LIB_FUNC("4uJJNi+C9wk", LibcInternal::isalnum);
	LIB_FUNC("srzSVSbKn7M", LibcInternal::isxdigit);
	LIB_FUNC("TYE4irxSmko", LibcInternal::toupper);
	LIB_FUNC("PqF+kHW-2WQ", LibcInternal::tolower);
	LIB_FUNC("oVkZ8W8-Q8A", LibcInternal::strtok);
	LIB_FUNC("2vDqwBlpF-o", LibcInternal::strtod);
	LIB_FUNC("xENtRue8dpI", LibcInternal::strtof);
	LIB_FUNC("VOBg+iNwB-4", LibcInternal::strtoll);
	LIB_FUNC("5OqszGpy7Mg", LibcInternal::strtoull);
	LIB_FUNC("SRI6S9B+-a4", LibcInternal::atof);
	LIB_FUNC("g7zzzLDYGw0", LibcInternal::strdup);
	LIB_FUNC("XGnuIBmEmyk", LibcInternal::strndup);
	LIB_FUNC("5jNubw4vlAA", LibcInternal::strnlen);
	LIB_FUNC("AV6ipCNa4Rw", LibcInternal::strcasecmp);
	LIB_FUNC("kHg45qPC6f0", LibcInternal::strncat);
	LIB_FUNC("kDZvoVssCgQ", LibcInternal::strpbrk);
	LIB_FUNC("q0F6yS-rCms", LibcInternal::strcspn);
	LIB_FUNC("-kU6bB4M-+k", LibcInternal::strspn);
	LIB_FUNC("RIa6GnWp+iU", LibcInternal::strerror);
	LIB_FUNC("RBcs3uut1TA", LibcInternal::strerror_r);
	LIB_FUNC("8u8lPzUEq+U", LibcInternal::memchr);
	LIB_FUNC("0uAUs3hYuG4", LibcInternal::ctime);

	// File I/O
	LIB_FUNC("xeYO4u7uyJ0", LibcInternal::fopen);
	LIB_FUNC("uodLYyUip20", LibcInternal::fclose);
	LIB_FUNC("lbB+UlZqVG0", LibcInternal::fread);
	LIB_FUNC("MpxhMh8QFro", LibcInternal::fwrite);
	LIB_FUNC("rQFVBXp-Cxg", LibcInternal::fseek);
	LIB_FUNC("pkYiKw09PRA", LibcInternal::fseeko);
	LIB_FUNC("Qazy8LmXTvw", LibcInternal::ftell);
	LIB_FUNC("5qP1iVQkdck", LibcInternal::ftello);
	LIB_FUNC("Fm-dmyywH9Q", LibcInternal::fileno);
	LIB_FUNC("LxcEU+ICu8U", LibcInternal::feof);
	LIB_FUNC("AHxyhN96dy4", LibcInternal::ferror);
	LIB_FUNC("QrZZdJ8XsX0", LibcInternal::fputs);
	LIB_FUNC("aZK8lNei-Qw", LibcInternal::fputc);
	LIB_FUNC("AEuF3F2f8TA", LibcInternal::fgetc);
	LIB_FUNC("KdP-nULpuGw", LibcInternal::fgets);
	LIB_FUNC("-LFO7jhD5CE", LibcInternal::ungetc);
	LIB_FUNC("QMFyLoqNxIg", LibcInternal::setvbuf);
	LIB_FUNC("3QIPIh-GDjw", LibcInternal::rewind);
	LIB_FUNC("m5wN+SwZOR4", LibcInternal::putchar);
	LIB_FUNC("8Q60JLJ6Rv4", LibcInternal::getc);

	// Formatted I/O
	LIB_FUNC("Q2V+iqvjgC0", LibcInternal::vsnprintf);
	LIB_FUNC("tcVi5SivF7Q", LibcInternal::sprintf);
	LIB_FUNC("pDBDcY6uLSA", LibcInternal::vfprintf);
	LIB_FUNC("fffwELXNVFA", LibcInternal::fprintf);

	// Misc / Environment
	LIB_FUNC("smbQukfxYJM", LibcInternal::getenv);
	LIB_FUNC("CRJcH8CnPSI", LibcInternal::unsetenv);
	LIB_FUNC("AEJdIVZTEmo", LibcInternal::qsort);
	LIB_FUNC("cpCOXWMgha0", LibcInternal::rand);
	LIB_FUNC("NesIgTmfF0Q", LibcInternal::bsearch);
	LIB_FUNC("MZO7FXyAPU8", LibcInternal::remove);
	LIB_FUNC("7Psx1DlAyE4", LibcInternal::getprogname);
	LIB_FUNC("PtsB1Q9wsFA", LibcInternal::setlocale);
	LIB_FUNC("1nTKA7pN1jw", LibcInternal::utime);

	// Wide Char
	LIB_FUNC("WkkeywLJcgU", LibcInternal::wcslen);
	LIB_FUNC("U7IhU4VEB-0", LibcInternal::iswprint);
	LIB_FUNC("vqtytrxgLMs", LibcInternal::iswspace);
	LIB_FUNC("wDmL2EH0CBs", LibcInternal::iswalnum);
	LIB_FUNC("D-qDARDb1aM", LibcInternal::iswalpha);
	LIB_FUNC("n0kT+8Eeizs", LibcInternal::iswdigit);
	LIB_FUNC("cjmSjRlnMAs", LibcInternal::iswxdigit);
	LIB_FUNC("AEPvEZkaLsU", LibcInternal::iswpunct);
	LIB_FUNC("1QcrrL9UDRQ", LibcInternal::iswupper);
	LIB_FUNC("Ok8KPy3nFls", LibcInternal::iswlower);
	LIB_FUNC("6A+1YZ79qFk", LibcInternal::iswcntrl);
	LIB_FUNC("p6DbM0OAHNo", LibcInternal::iswblank);
	LIB_FUNC("wjG0GyCyaP0", LibcInternal::iswgraph);
	LIB_FUNC("1uf1SQsj5go", LibcInternal::towupper);
	LIB_FUNC("J3J1T9fjUik", LibcInternal::towlower);
	LIB_FUNC("fnUEjBCNRVU", LibcInternal::wmemchr);
	LIB_FUNC("QJ5xVfKkni0", LibcInternal::wmemcmp);
	LIB_FUNC("qVHpv0PxouI", LibcInternal::mbrtowc);
	LIB_FUNC("y9OoA+P5cjk", LibcInternal::wcrtomb);
	LIB_FUNC("6eU9xX9oEdQ", LibcInternal::mbtowc);
	LIB_FUNC("LEvm25Sxi7I", LibcInternal::btowc);
	LIB_FUNC("CL7VJxznu6g", LibcInternal::wctob);
	LIB_FUNC("+3PtYiUxl-U", LibcInternal::wctype);
	LIB_FUNC("45E7omS0vvc", LibcInternal::iswctype);

	// Global Objects
	LIB_OBJECT("zqJhBxAKfsc", &LibcInternal::g_stdout); // __stdoutp
	LIB_OBJECT("as8Od-tH1BI", &LibcInternal::g_stderr); // __stderrp
	LIB_OBJECT("bgAcsbcEznc", &LibcInternal::g_stdin);  // __stdinp
	LIB_OBJECT("q1OvUam0BJU", &LibcInternal::g_isthreaded); // __isthreaded
	LIB_OBJECT("77pL1FoD4I4", &LibcInternal::g_mb_cur_max); // __mb_cur_max

	LIB_FUNC("L1SBTkC+Cvw", LibC::abort);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::std_execute_once);

	LIB_FUNC("al3JzFI9MQ0", LibcInternal::LibcHeapErrorReportForGame);
}

} // namespace LibcInternal

namespace LibC {

LIB_DEFINE(InitLibC_1) {
	LibcInternal::InitLibcInternal_1(s);

	LIB_OBJECT("P330P3dFF68", &LibC::g_need_flag);

	LIB_FUNC("uMei1W9uyNo", LibC::exit);
	LIB_FUNC("L1SBTkC+Cvw", LibC::abort);
	LIB_FUNC("9BcDykPmo1I", LibC::libc_error);
	LIB_FUNC("bzQExy189ZI", LibC::init_env);
	LIB_FUNC("8G2LB+A3rzg", LibC::atexit);
	LIB_FUNC("hcuQgD53UxM", LibC::libc_printf);
	LIB_FUNC("YQ0navp+YIc", LibC::puts);
	LIB_FUNC("M4YYbSFfJ8g", LibC::setenv);
	LIB_FUNC("wLlFkwG9UcQ", LibC::libc_time);
	LIB_FUNC("-VVn74ZyhEs", LibC::libc_difftime);
	LIB_FUNC("1mecP7RgI2A", LibC::libc_gmtime);
	LIB_FUNC("efhK-YSUYYQ", LibC::libc_localtime);
	LIB_FUNC("n7AepwR0s34", LibC::libc_mktime);
	LIB_FUNC("Av3zjWi64Kw", LibC::libc_strftime);
	LIB_FUNC("XKRegsFpEpk", LibC::catchReturnFromMain);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::std_execute_once);
}

} // namespace LibC

} // namespace Libs
