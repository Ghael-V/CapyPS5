// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "cpu/cpuBackend.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/memory.h"
#if defined(__ANDROID__)
#include "android/capyps5_jni.h"
#include <sys/stat.h>
#endif
#include "kernel/pthread.h"

#include <vector>

#if !defined(__x86_64__) && !defined(_M_X64)
#include "common/virtualMemory.h"
#include "loader/runtimeLinker.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AddressFold.h>
#include <FEXCore/fextl/memory.h>
#if defined(__aarch64__)
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <ucontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <dirent.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace FEXCore::Allocator {
void InitializeAllocator(size_t PageSize);
void InitializeThread();
void* aligned_alloc(size_t a, size_t s);
void aligned_free(void* ptr);
}
extern "C" int rpmalloc_is_thread_initialized(void);
#endif

#if defined(__ANDROID__) || defined(ANDROID)
#include <android/log.h>
// Info logs sit on hot paths (every guest callback); only emit them in verbose mode.
#define ALOGI(...)                                                                                 	do {                                                                                           		if (Common::DebugFlags::Verbose()) {                                                       			__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Cpu", __VA_ARGS__);                     		}                                                                                          	} while (0)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "CapyPS5-Cpu", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Cpu", __VA_ARGS__)
#else
#define ALOGI(...) std::printf(__VA_ARGS__)
#define ALOGW(...) std::printf(__VA_ARGS__)
#define ALOGE(...) std::fprintf(stderr, __VA_ARGS__)
#endif

namespace Libs::LibKernel {
bool KernelHasPendingSignals();
void KernelDispatchPendingSignalForCurrentThread();
} // namespace Libs::LibKernel

namespace {
// `setprop debug.capy.hle_stats 1`: per (thread, HLE function) call count and time spent inside,
// logged every 3 s under CapyPS5-Hle (function as offset into the library, for addr2line).
struct HleStat {
	uint64_t func  = 0;
	int      tid   = 0;
	uint64_t calls = 0;
	uint64_t ns    = 0;
};
// Call each thread is currently inside (logged with the stats, to catch calls that block long).
struct HleInFlight {
	std::atomic<uint64_t> func {0};
	std::atomic<int64_t>  start_ns {0};
	std::atomic<uint64_t> guest_rsp {0};
	int                   tid = 0;
};
std::mutex                g_hle_inflight_mutex;
std::vector<HleInFlight*> g_hle_inflight;

HleInFlight& CurrentHleInFlight() {
	thread_local HleInFlight* slot = [] {
		auto* created = new HleInFlight;
		created->tid  = static_cast<int>(gettid());
		std::lock_guard lock(g_hle_inflight_mutex);
		g_hle_inflight.push_back(created);
		return created;
	}();
	return *slot;
}

int64_t HleNowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

thread_local uint64_t t_last_hle_func = 0;

std::mutex           g_hle_stats_mutex;
std::vector<HleStat> g_hle_stats;
int64_t              g_hle_stats_next_ms = 0;

bool HleStatsEnabled() {
	static Common::DebugFlags::CachedValue enabled("debug.capy.hle_stats", 0);
	return enabled.Get() != 0;
}

void HleDumpInFlight();

// `setprop debug.capy.dump_stacks N` (with hle_stats on): every time N changes, log a host
// backtrace of every thread (tag CapyPS5-Stack). Walks the AArch64 frame-pointer chain from the
// signal context; frames are read with process_vm_readv so a bogus x29 (JIT code keeps a guest
// register there) cannot fault. Debugging aid for hangs.
#if defined(__aarch64__) && defined(__ANDROID__)
void StackDumpHandler(int /*signal*/, siginfo_t* /*info*/, void* context) {
	const auto* uc = static_cast<const ucontext_t*>(context);
	char        name[32] = {};
	pthread_getname_np(pthread_self(), name, sizeof(name));
	char line[1600];
	int  used = std::snprintf(line, sizeof(line), "tid %d [%s]:", static_cast<int>(gettid()), name);
	const auto add = [&](uint64_t address) {
		if (used >= static_cast<int>(sizeof(line)) - 64) {
			return;
		}
		Dl_info info {};
		if (dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fbase != nullptr) {
			const char* file = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
			used += std::snprintf(line + used, sizeof(line) - used, " %s+0x%llx",
			                      file != nullptr ? file + 1 : "?",
			                      static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(info.dli_fbase)));
		} else {
			used += std::snprintf(line + used, sizeof(line) - used, " 0x%llx", static_cast<unsigned long long>(address));
		}
	};
	add(uc->uc_mcontext.pc);
	add(uc->uc_mcontext.regs[30]);
	uint64_t fp = uc->uc_mcontext.regs[29];
	for (int depth = 0; depth < 28 && fp != 0 && (fp & 7u) == 0; depth++) {
		uint64_t frame[2] = {};
		iovec    local {frame, sizeof(frame)};
		iovec    remote {reinterpret_cast<void*>(fp), sizeof(frame)};
		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(sizeof(frame)) ||
		    frame[1] == 0) {
			break;
		}
		add(frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	__android_log_write(ANDROID_LOG_WARN, "CapyPS5-Stack", line);
}

void DumpAllThreadStacks() {
	const int signal_number = SIGRTMAX - 2;
	static std::once_flag installed;
	std::call_once(installed, [signal_number] {
		struct sigaction action {};
		action.sa_sigaction = &StackDumpHandler;
		action.sa_flags     = SA_SIGINFO | SA_RESTART;
		sigemptyset(&action.sa_mask);
		sigaction(signal_number, &action, nullptr);
	});
	if (auto* dir = opendir("/proc/self/task"); dir != nullptr) {
		while (auto* entry = readdir(dir)) {
			const int tid = std::atoi(entry->d_name);
			if (tid > 0 && tid != static_cast<int>(gettid())) {
				syscall(SYS_tgkill, getpid(), tid, signal_number);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		closedir(dir);
	}
}
#endif

// With hle_stats on, report in-flight calls every 3 s even when every guest thread is blocked
// (a hang inside one HLE call makes no further calls that would print the stats).
void HleStartWatchdog() {
	static std::once_flag once;
	std::call_once(once, [] {
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				if (HleStatsEnabled()) {
					HleDumpInFlight();
#if defined(__aarch64__) && defined(__ANDROID__)
					static Common::DebugFlags::CachedValue dump_stacks("debug.capy.dump_stacks", 0);
					static int64_t                         dumped = 0;
					if (dump_stacks.Get() != dumped) {
						dumped = dump_stacks.Get();
						if (dumped != 0) {
							DumpAllThreadStacks();
						}
					}
#endif
				}
			}
		}).detach();
	});
}

void HleDumpInFlight() {
	Dl_info info {};
	const auto base = dladdr(reinterpret_cast<void*>(&HleDumpInFlight), &info) != 0
	                      ? reinterpret_cast<uint64_t>(info.dli_fbase)
	                      : 0;
	{
		const auto now_ns = HleNowNs();
		std::lock_guard inflight_lock(g_hle_inflight_mutex);
		for (const auto* slot: g_hle_inflight) {
			const auto func  = slot->func.load(std::memory_order_relaxed);
			const auto start = slot->start_ns.load(std::memory_order_relaxed);
			if (func != 0 && now_ns - start > 100000000) {
				__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Hle", "in flight: tid %d fn +0x%llx for %.0f ms",
				                    slot->tid, static_cast<unsigned long long>(func - base),
				                    static_cast<double>(now_ns - start) / 1e6);
				// Guest code addresses found on the guest stack (a rough backtrace).
				const auto stack_addr = slot->guest_rsp.load(std::memory_order_relaxed);
				// Copy with process_vm_readv so a short stack cannot fault the watchdog.
				static uint64_t stack_copy[4096];
				size_t          stack_words = 0;
				if (stack_addr != 0) {
					for (size_t chunk = 0; chunk < 4096; chunk += 512) {
						iovec local {stack_copy + chunk, 512 * sizeof(uint64_t)};
						iovec remote {reinterpret_cast<void*>(stack_addr + chunk * sizeof(uint64_t)), 512 * sizeof(uint64_t)};
						if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(local.iov_len)) {
							break;
						}
						stack_words = chunk + 512;
					}
				}
				const auto* stack = stack_copy;
				if (stack_words != 0) {
					char line[1024] = {};
					int  used = 0;
					for (size_t n = 0; n < stack_words && used < 1000; n++) {
						const auto value = stack[n];
						if (value >= 0x800000000ull && value < 0x980000000ull) {
							used += std::snprintf(line + used, sizeof(line) - used, " %llx", static_cast<unsigned long long>(value));
						}
					}
					__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Hle", "  guest stack:%s", line);
				}
			}
		}
	}
}

void HleStatsRecord(uint64_t func, uint64_t ns) {
	const int tid = static_cast<int>(gettid());
	std::lock_guard lock(g_hle_stats_mutex);
	bool found = false;
	for (auto& stat: g_hle_stats) {
		if (stat.func == func && stat.tid == tid) {
			stat.calls++;
			stat.ns += ns;
			found = true;
			break;
		}
	}
	if (!found && g_hle_stats.size() < 4096) {
		g_hle_stats.push_back({func, tid, 1, ns});
	}
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	if (g_hle_stats_next_ms == 0) {
		g_hle_stats_next_ms = now + 3000;
	}
	if (now < g_hle_stats_next_ms) {
		return;
	}
	g_hle_stats_next_ms = now + 3000;
	std::sort(g_hle_stats.begin(), g_hle_stats.end(),
	          [](const HleStat& a, const HleStat& b) { return a.ns > b.ns; });
	Dl_info info {};
	const auto base = dladdr(reinterpret_cast<void*>(&HleStatsRecord), &info) != 0
	                      ? reinterpret_cast<uint64_t>(info.dli_fbase)
	                      : 0;
	for (size_t n = 0; n < g_hle_stats.size() && n < 25; n++) {
		const auto& stat = g_hle_stats[n];
		char name[32] = {};
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", stat.tid);
		if (auto* file = std::fopen(path, "r"); file != nullptr) {
			if (std::fgets(name, sizeof(name), file) != nullptr) {
				name[std::strcspn(name, "\n")] = 0;
			}
			std::fclose(file);
		}
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Hle", "tid %d [%s] fn +0x%llx calls %llu ms %.1f",
		                    stat.tid, name, static_cast<unsigned long long>(stat.func - base),
		                    static_cast<unsigned long long>(stat.calls), static_cast<double>(stat.ns) / 1e6);
	}
	// Spinning threads make many short calls and never reach the by-time list.
	std::sort(g_hle_stats.begin(), g_hle_stats.end(),
	          [](const HleStat& a, const HleStat& b) { return a.calls > b.calls; });
	for (size_t n = 0; n < g_hle_stats.size() && n < 10; n++) {
		const auto& stat = g_hle_stats[n];
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Hle", "by calls: tid %d fn +0x%llx calls %llu ms %.1f",
		                    stat.tid, static_cast<unsigned long long>(stat.func - base),
		                    static_cast<unsigned long long>(stat.calls), static_cast<double>(stat.ns) / 1e6);
	}
	HleDumpInFlight();
	__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Hle", "----");
	g_hle_stats.clear();
}
} // namespace

namespace Cpu {

#if defined(__x86_64__) || defined(_M_X64)

class NativeCpuBackend final : public CpuBackend {
public:
	BackendType GetType() const override { return BackendType::Native; }
	const char* GetName() const override { return "Native (x86_64)"; }

	bool Initialize(uint64_t /*guest_base*/, uint64_t /*guest_size*/) override {
		return true;
	}

	void Shutdown() override {}

	void RunEntry(uint64_t addr, Loader::EntryParams* params,
	              Loader::atexit_func_t atexit_func, void* stack_top) override {
		using entry_func_t = KYTY_SYSV_ABI void (*)(Loader::EntryParams* params, Loader::atexit_func_t atexit_func);
		auto* func = reinterpret_cast<entry_func_t>(addr);

		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		uintptr_t guest_root_frame[2] = {};
		(void)guest_rsp;

#if defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "movq %%rbp, %%r12\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r12, %%rbp\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1",
		               "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");
#else
		asm volatile("movq %%rbp, %%r12\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r12, %%rbp\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0",
		               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
		               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
	}

	int RunModuleInit(uint64_t addr, size_t args, const void* argp,
	                  Loader::module_func_t func) override {
		using module_ini_fini_func_t =
		    KYTY_SYSV_ABI int (*)(size_t args, const void* argp, Loader::module_func_t func);
		return reinterpret_cast<module_ini_fini_func_t>(addr)(args, argp, func);
	}

	int RunModuleFini(uint64_t addr, size_t args, const void* argp,
	                  Loader::module_func_t func) override {
		using module_ini_fini_func_t =
		    KYTY_SYSV_ABI int (*)(size_t args, const void* argp, Loader::module_func_t func);
		return reinterpret_cast<module_ini_fini_func_t>(addr)(args, argp, func);
	}

	void* RunOnGuestStack(void* arg, Libs::LibKernel::pthread_entry_func_t func,
	                      void* stack_top) override {
		void*      ret = nullptr;
		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
		guest_root_frame[0]    = 0;
		guest_root_frame[1]    = 0;

		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "pushq %%r14\n\t"
		             "pushq %%r15\n\t"
		             "movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%%rsi\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             "popq %%r15\n\t"
		             "popq %%r14\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             : "=a"(ret), "+D"(arg), "+S"(func)
		             : [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
		             : "cc", "memory", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
		               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");

		return ret;
	}

	uint64_t CallGuestCallback(uint64_t guest_func_ptr, uint64_t arg0 = 0,
	                           uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0,
	                           uint64_t arg4 = 0, uint64_t arg5 = 0) override {
		using callback_fn_t =
		    KYTY_SYSV_ABI uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
		return reinterpret_cast<callback_fn_t>(guest_func_ptr)(arg0, arg1, arg2, arg3, arg4, arg5);
	}
};

#else // Architecture is ARM64 or other non-x86_64 host

namespace {

#if defined(__aarch64__)
thread_local FEXCore::Core::InternalThreadState* s_current_fex_thread = nullptr;

// Guest address folding (kernel/memory.cpp): start masking guest addresses in translated code.
FEXCore::Context::Context* s_fold_context = nullptr;

void EnableGuestAddressFold(uint64_t mask) {
	FEXCore::GuestAddressFoldMask.store(mask, std::memory_order_relaxed);
	// Code translated so far does not mask; drop it so it is translated again. Unreal reserves its
	// range during start-up, while only the main thread runs guest code.
	auto* thread = s_current_fex_thread;
	if (s_fold_context != nullptr && thread != nullptr) {
		std::unique_lock lock(s_fold_context->GetCodeInvalidationMutex());
		s_fold_context->InvalidateCodeBuffersCodeRange(0, 1ull << 47u);
		s_fold_context->InvalidateThreadCachedCodeRange(thread, 0, 1ull << 47u);
	}
	ALOGW("Guest address folding enabled (mask 0x%llx)", static_cast<unsigned long long>(mask));
}
#endif
// Counts ExecuteGuestCode runs on this host thread, so an HLE call can tell whether it ran
// guest code nested inside it (e.g. sceKernelLoadStartModule running module_start).
thread_local uint64_t s_nested_guest_runs = 0;

// Caller state of the HLE call running on this thread, captured only when that call goes on to
// run nested guest code (which reuses the frame). Copying ~1 KiB on every HLE call cost the
// Unreal render thread ~10 %.
struct HleSavedRegisters {
	FEXCore::Core::CpuStateFrame* frame  = nullptr;
	bool                          filled = false;
	uint64_t                      gregs[16];
	uint64_t                      avx_high[16][2];
	decltype(FEXCore::Core::CPUState::xmm) xmm;
	uint64_t                      mm[8][2];
	uint8_t                       flags[48];
	uint32_t                      pf_raw, af_raw, mxcsr;
	uint64_t                      fs_cached, gs_cached, callret_sp;
};
thread_local HleSavedRegisters* t_hle_saved = nullptr;

void CaptureHleCallerState() {
	auto* saved = t_hle_saved;
	if (saved == nullptr || saved->filled || saved->frame == nullptr) {
		return;
	}
	const auto& state = saved->frame->State;
	std::memcpy(saved->gregs, state.gregs, sizeof(saved->gregs));
	std::memcpy(saved->avx_high, state.avx_high, sizeof(saved->avx_high));
	std::memcpy(&saved->xmm, &state.xmm, sizeof(saved->xmm));
	std::memcpy(saved->mm, state.mm, sizeof(saved->mm));
	std::memcpy(saved->flags, state.flags, sizeof(saved->flags));
	saved->pf_raw     = state.pf_raw;
	saved->af_raw     = state.af_raw;
	saved->mxcsr      = state.mxcsr;
	saved->fs_cached  = state.fs_cached;
	saved->gs_cached  = state.gs_cached;
	saved->callret_sp = state.callret_sp;
	saved->filled     = true;
}

std::mutex s_trampoline_mutex;
std::unordered_map<uint64_t, uint64_t> s_trampoline_cache;
std::unordered_map<uint64_t, std::string> s_trampoline_names;
uint8_t* s_trampoline_pool = nullptr;
size_t s_trampoline_pool_used = 0;
constexpr size_t TRAMPOLINE_POOL_SIZE = 4 * 1024 * 1024; // 4 MB

void EnsureTrampolinePool() {
	if (s_trampoline_pool == nullptr) {
		s_trampoline_pool = static_cast<uint8_t*>(
		    mmap(nullptr, TRAMPOLINE_POOL_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (s_trampoline_pool == MAP_FAILED || s_trampoline_pool == nullptr) {
			ALOGE("CpuBackend: Failed to allocate trampoline pool! errno=%d", errno);
			std::abort();
		}

		// Slot 0 (offset 0..15): HLT Sentinel for clean JIT execution return
		// 0xF4 = HLT, followed by NOPs (0x90)
		s_trampoline_pool[0] = 0xF4;
		std::memset(&s_trampoline_pool[1], 0x90, 15);

		// Slot 1 (offset 16..31): CALLBACKRET for re-entrant guest callback returns
		// 0x0F, 0x3E = FEX CALLBACKRET opcode
		s_trampoline_pool[16] = 0x0F;
		s_trampoline_pool[17] = 0x3E;
		std::memset(&s_trampoline_pool[18], 0x90, 14);

		Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(s_trampoline_pool), 32);

		s_trampoline_pool_used = 32;
	}
}

class CapySyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
	void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
#if defined(__aarch64__)
		s_current_fex_thread = Frame->Thread;
#endif
		uint64_t host_func = Frame->State.gregs[FEXCore::X86State::REG_RAX];
		uint64_t call_rip  = Frame->State.rip;
		if (host_func >= 0x10000) {
			// HLE Trampoline dispatch
			uint64_t a0 = Frame->State.gregs[FEXCore::X86State::REG_RDI];
			uint64_t a1 = Frame->State.gregs[FEXCore::X86State::REG_RSI];
			uint64_t a2 = Frame->State.gregs[FEXCore::X86State::REG_RDX];
			uint64_t a3 = Frame->State.gregs[FEXCore::X86State::REG_R10]; // Preserved from RCX!
			uint64_t a4 = Frame->State.gregs[FEXCore::X86State::REG_R8];
			uint64_t a5 = Frame->State.gregs[FEXCore::X86State::REG_R9];

			// Guest stack arguments if function takes > 6 arguments
			uint64_t* guest_stack = reinterpret_cast<uint64_t*>(Frame->State.gregs[FEXCore::X86State::REG_RSP]);
			uint64_t a6  = guest_stack ? guest_stack[1] : 0;
			uint64_t a7  = guest_stack ? guest_stack[2] : 0;
			uint64_t a8  = guest_stack ? guest_stack[3] : 0;
			uint64_t a9  = guest_stack ? guest_stack[4] : 0;
			uint64_t a10 = guest_stack ? guest_stack[5] : 0;
			uint64_t a11 = guest_stack ? guest_stack[6] : 0;
			uint64_t a12 = guest_stack ? guest_stack[7] : 0;
			uint64_t a13 = guest_stack ? guest_stack[8] : 0;
			uint64_t a14 = guest_stack ? guest_stack[9] : 0;
			uint64_t a15 = guest_stack ? guest_stack[10] : 0;
			if (Libs::LibKernel::Memory::HasFoldWindows()) {
				// Pointers into guest ranges above the 39-bit host limit reach emulator code
				// unmasked (the JIT only masks the guest's own accesses).
				for (auto* arg: {&a0, &a1, &a2, &a3, &a4, &a5, &a6, &a7, &a8, &a9, &a10, &a11, &a12,
				                 &a13, &a14, &a15}) {
					*arg = Libs::LibKernel::Memory::FoldHleArgument(*arg);
				}
			}

			using hle_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t);
			auto fn = reinterpret_cast<hle_fn_t>(host_func);
			// Nested guest code run by the HLE function leaves its registers in this frame
			// (Unity games crashed after sceKernelLoadStartModule with RBP = module_start's
			// argc). Restore the caller's full CPU state when that happened.
			HleSavedRegisters saved;
			saved.frame               = Frame;
			auto* const outer_saved   = t_hle_saved;
			t_hle_saved               = &saved;
			auto&       state         = Frame->State;
			const bool hle_stats = HleStatsEnabled();
			if (hle_stats) {
				HleStartWatchdog();
			}
			// In-flight tracking for the hang watchdog: only while hle_stats is on (clock reads on
			// every HLE call were measurable).
			HleInFlight* in_flight   = hle_stats ? &CurrentHleInFlight() : nullptr;
			uint64_t     outer_func  = 0;
			int64_t      outer_start = 0;
			if (in_flight != nullptr) {
				outer_func  = in_flight->func.load(std::memory_order_relaxed);
				outer_start = in_flight->start_ns.load(std::memory_order_relaxed);
				in_flight->start_ns.store(HleNowNs(), std::memory_order_relaxed);
				in_flight->guest_rsp.store(Frame->State.gregs[FEXCore::X86State::REG_RSP], std::memory_order_relaxed);
				in_flight->func.store(host_func, std::memory_order_relaxed);
			}
			const auto hle_start = hle_stats ? std::chrono::steady_clock::now()
			                                 : std::chrono::steady_clock::time_point {};
			uint64_t ret = fn(a0, a1, a2, a3, a4, a5, a6, a7,
			                  a8, a9, a10, a11, a12, a13, a14, a15);
			t_last_hle_func = host_func;
			t_hle_saved     = outer_saved;
			{
				// `setprop debug.capy.hle_trace N` (before launching a game): log the first N HLE
				// calls with their first arguments and result (tag CapyPS5-HleTrace), to see what a
				// game did before it failed.
				static Common::DebugFlags::CachedValue hle_trace("debug.capy.hle_trace", 0);
				static std::atomic<int64_t>             traced {0};
				if (hle_trace.Get() > 0 && traced.load(std::memory_order_relaxed) < hle_trace.Get()) {
					std::string name = "?";
					{
						std::lock_guard<std::mutex> lock(s_trampoline_mutex);
						if (const auto it = s_trampoline_names.find(host_func); it != s_trampoline_names.end()) {
							name = it->second;
						}
					}
					// Mutex calls drown everything else; open/stat-like calls show their path.
					if (name.find("Mutex") == std::string::npos && name.find("mutex") == std::string::npos &&
					    traced.fetch_add(1, std::memory_order_relaxed) < hle_trace.Get()) {
						char path[96] = {};
						if (name.find("open") != std::string::npos || name.find("Open") != std::string::npos ||
						    name.find("stat") != std::string::npos || name.find("Stat") != std::string::npos) {
							for (size_t i = 0; i + 1 < sizeof(path); i++) {
								char c = 0;
								if (!Libs::LibKernel::Memory::ReadGuestMemory(a0 + i, &c, 1) || c == 0) {
									break;
								}
								path[i] = c;
							}
						}
						__android_log_print(ANDROID_LOG_WARN, "CapyPS5-HleTrace",
						                    "tid %d %s(0x%llx, 0x%llx, 0x%llx) = 0x%llx %s", static_cast<int>(gettid()),
						                    name.c_str(), static_cast<unsigned long long>(a0),
						                    static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
						                    static_cast<unsigned long long>(ret), path);
					}
				}
			}
			if (in_flight != nullptr) {
				in_flight->func.store(outer_func, std::memory_order_relaxed);
				in_flight->start_ns.store(outer_start, std::memory_order_relaxed);
			}
			if (hle_stats) {
				HleStatsRecord(host_func, static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(
				        std::chrono::steady_clock::now() - hle_start).count()));
			}
			if (saved.filled) {
				static std::atomic<uint32_t> restored_log {0};
				if (restored_log.fetch_add(1) < 8) {
					__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Cpu",
					    "HLE call ran nested guest code: restored caller registers (rbp 0x%llx -> 0x%llx)",
					    static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RBP]),
					    static_cast<unsigned long long>(saved.gregs[FEXCore::X86State::REG_RBP]));
				}
				std::memcpy(state.gregs, saved.gregs, sizeof(saved.gregs));
				std::memcpy(state.avx_high, saved.avx_high, sizeof(saved.avx_high));
				std::memcpy(&state.xmm, &saved.xmm, sizeof(saved.xmm));
				std::memcpy(state.mm, saved.mm, sizeof(saved.mm));
				std::memcpy(state.flags, saved.flags, sizeof(saved.flags));
				state.pf_raw = saved.pf_raw;
				state.af_raw = saved.af_raw;
				state.mxcsr = saved.mxcsr;
				state.fs_cached = saved.fs_cached;
				state.gs_cached = saved.gs_cached;
				state.callret_sp = saved.callret_sp;
			}
#if defined(__aarch64__)
			s_current_fex_thread = Frame->Thread;
#endif

			Frame->State.gregs[FEXCore::X86State::REG_RAX] = ret;
			// Deliver guest signals (sceKernelRaiseException) queued for this thread while it
			// runs JIT code; blocking HLE waits poll on their own.
			if (Libs::LibKernel::KernelHasPendingSignals()) {
				Frame->State.rip = call_rip + 2;
				Libs::LibKernel::KernelDispatchPendingSignalForCurrentThread();
				Frame->State.gregs[FEXCore::X86State::REG_RAX] = ret;
			}
		} else {
			ALOGW("CapySyscallHandler: Raw syscall %llu requested", static_cast<unsigned long long>(host_func));
			Frame->State.gregs[FEXCore::X86State::REG_RAX] = static_cast<uint64_t>(-1);
		}

		// Advance RIP past the 2-byte syscall instruction (0x0F 0x05)
		// Preserving call_rip in case nested guest callbacks modified Frame->State.rip!
		Frame->State.rip = call_rip + 2;
	}

	FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* /*Thread*/, uint64_t /*Address*/) override {
		return FEXCore::HLE::ExecutableRangeInfo {
			.Base = 0,
			.Size = 0x0000800000000000ULL, // 128TB
			.Writable = true,
		};
	}

	std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState* /*Thread*/, uint64_t /*GuestAddr*/) override {
		return std::nullopt;
	}
};

class CapySignalDelegator final : public FEXCore::SignalDelegator {
public:
	uintptr_t GetThunkCallbackRET() const override {
		EnsureTrampolinePool();
		return reinterpret_cast<uintptr_t>(s_trampoline_pool + 16);
	}
};

} // anonymous namespace

uint64_t GetOrCreateHleTrampoline(uint64_t host_func_ptr, const char* name) {
	if (host_func_ptr == 0) {
		return 0;
	}

	std::lock_guard<std::mutex> lock(s_trampoline_mutex);
	if (name != nullptr) {
		s_trampoline_names[host_func_ptr] = name;
	}
	EnsureTrampolinePool();

	auto it = s_trampoline_cache.find(host_func_ptr);
	if (it != s_trampoline_cache.end()) {
		return it->second;
	}

	if (s_trampoline_pool_used + 16 > TRAMPOLINE_POOL_SIZE) {
		ALOGE("CpuBackend: Trampoline pool exhausted!");
		std::abort();
	}

	uint8_t* stub = s_trampoline_pool + s_trampoline_pool_used;
	s_trampoline_pool_used += 16;

	// mov r10, rcx (3 bytes: 49 89 ca) - preserve arg 4 before syscall
	stub[0] = 0x49;
	stub[1] = 0x89;
	stub[2] = 0xCA;
	// movabs rax, imm64 (10 bytes: 48 b8 <8-byte pointer>)
	stub[3] = 0x48;
	stub[4] = 0xB8;
	std::memcpy(&stub[5], &host_func_ptr, 8);
	// syscall (2 bytes: 0f 05)
	stub[13] = 0x0F;
	stub[14] = 0x05;
	// ret (1 byte: c3)
	stub[15] = 0xC3;

	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(stub), 16);

	uint64_t stub_addr = reinterpret_cast<uint64_t>(stub);
	s_trampoline_cache[host_func_ptr] = stub_addr;
	return stub_addr;
}

class Arm64CpuBackend final : public CpuBackend {
public:
	BackendType GetType() const override { return BackendType::Dynarec; }
	const char* GetName() const override { return "Dynarec (FEXCore ARM64)"; }

	bool Initialize(uint64_t guest_base, uint64_t guest_size) override {
		if (m_fex_context) {
			return true;
		}
		m_guest_base = guest_base;
		m_guest_size = guest_size;

		FEXCore::Allocator::InitializeAllocator(4096);
		FEXCore::Allocator::InitializeThread();

		EnsureTrampolinePool();

		FEXCore::Config::Initialize();
		FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
		FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
		FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "0");
		FEXCore::Config::Set(FEXCore::Config::CONFIG_SILENTLOG, "1");
		FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");
		// `setprop debug.capy.dump_ir 1`: dump every block's final IR to <files>/ir (debugging only).
		static Common::DebugFlags::CachedValue dump_ir("debug.capy.dump_ir", 0);
		if (dump_ir.Get() != 0) {
			const auto dir = AndroidGetFilesDir() + "/ir";
			::mkdir(dir.c_str(), 0700);
			FEXCore::Config::Set(FEXCore::Config::CONFIG_DUMPIR, dir.c_str());
			FEXCore::Config::Set(FEXCore::Config::CONFIG_PASSMANAGERDUMPIR, "afteropt");
		}

		FEXCore::HostFeatures features {};
		features.DCacheLineLog2 = 6;
		features.SupportsCacheMaintenanceOps = 1;
		features.SupportsAES = 1;
		// PS5 games use SHA-NI unconditionally (Guri: sha1msg1); FEX translates it to the ARMv8
		// SHA1/SHA256 instructions every supported Snapdragon has.
		features.SupportsSHA = 1;
		features.SupportsCRC = 1;
		features.SupportsAtomics = 1;
		features.SupportsRCPC = 1;
		features.SupportsAVX = 1; // AVX 128-bit vector support for Celeste PICO-8
		features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
		features.ProcessPID = ::getpid();

		m_fex_context = FEXCore::Context::Context::CreateNewContext(features);
		if (!m_fex_context) {
			ALOGE("Arm64CpuBackend: Failed to create FEXCore context!");
			return false;
		}

		m_fex_context->SetSyscallHandler(&m_syscall_handler);
		m_fex_context->SetSignalDelegator(&m_signal_delegator);
		m_fex_context->EnableExitOnHLT();

		if (!m_fex_context->InitCore()) {
			ALOGE("Arm64CpuBackend: Failed to initialize FEXCore core!");
			return false;
		}

		s_fold_context = m_fex_context.get();
		Libs::LibKernel::Memory::SetAddressFoldHook(&EnableGuestAddressFold);
		// On a 39-bit host, mask guest addresses from the start: translated code cannot be safely
		// invalidated later from inside an HLE call. Only addresses the host cannot map change.
		{
			constexpr uint64_t FOLD_MASK = (1ull << 39u) - 1u;
			int                probe     = 0;
			static Common::DebugFlags::CachedValue address_fold("debug.capy.address_fold", 1);
			if (address_fold.Get() != 0 && reinterpret_cast<uintptr_t>(&probe) <= FOLD_MASK) {
				FEXCore::GuestAddressFoldMask.store(FOLD_MASK, std::memory_order_relaxed);
				Libs::LibKernel::Memory::SetAddressFoldMask(FOLD_MASK);
				ALOGI("Arm64CpuBackend: 39-bit host, guest address folding on");
			}
		}

		ALOGI("Arm64CpuBackend: Initialized FEXCore JIT (guest base=0x%016llx, size=0x%016llx)",
		      static_cast<unsigned long long>(guest_base), static_cast<unsigned long long>(guest_size));
		return true;
	}

	void Shutdown() override {
		m_fex_context.reset();
		FEXCore::Config::Shutdown();
		ALOGI("Arm64CpuBackend: Shutdown");
	}

	void RunEntry(uint64_t addr, Loader::EntryParams* params,
	              Loader::atexit_func_t atexit_func, void* stack_top) override {
		ALOGI("Arm64CpuBackend: RunEntry at 0x%016llx, params=%p, atexit=%p, stack_top=%p",
		      static_cast<unsigned long long>(addr), static_cast<void*>(params),
		      reinterpret_cast<void*>(atexit_func), stack_top);

		uint64_t guest_atexit = atexit_func ? GetOrCreateHleTrampoline(reinterpret_cast<uint64_t>(atexit_func), "atexit") : 0;
		ExecuteGuestCode(addr, reinterpret_cast<uint64_t>(params), guest_atexit, 0, reinterpret_cast<uint64_t>(stack_top));
	}

	int RunModuleInit(uint64_t addr, size_t args, const void* argp,
	                  Loader::module_func_t func) override {
		ALOGI("Arm64CpuBackend: RunModuleInit at 0x%016llx, args=%zu, argp=%p",
		      static_cast<unsigned long long>(addr), args, argp);

		uint64_t guest_func = func ? GetOrCreateHleTrampoline(reinterpret_cast<uint64_t>(func), "module_func") : 0;
		return static_cast<int>(ExecuteGuestCode(addr, static_cast<uint64_t>(args),
		                                         reinterpret_cast<uint64_t>(argp),
		                                         guest_func, 0));
	}

	int RunModuleFini(uint64_t addr, size_t args, const void* argp,
	                  Loader::module_func_t func) override {
		ALOGI("Arm64CpuBackend: RunModuleFini at 0x%016llx, args=%zu, argp=%p",
		      static_cast<unsigned long long>(addr), args, argp);

		uint64_t guest_func = func ? GetOrCreateHleTrampoline(reinterpret_cast<uint64_t>(func), "module_func") : 0;
		return static_cast<int>(ExecuteGuestCode(addr, static_cast<uint64_t>(args),
		                                         reinterpret_cast<uint64_t>(argp),
		                                         guest_func, 0));
	}

	void* RunOnGuestStack(void* arg, Libs::LibKernel::pthread_entry_func_t func,
	                      void* stack_top) override {
		ALOGI("Arm64CpuBackend: RunOnGuestStack func=%p, arg=%p, stack_top=%p",
		      reinterpret_cast<void*>(func), arg, stack_top);

		uint64_t ret = ExecuteGuestCode(reinterpret_cast<uint64_t>(func),
		                                reinterpret_cast<uint64_t>(arg),
		                                0, 0, reinterpret_cast<uint64_t>(stack_top));
		return reinterpret_cast<void*>(ret);
	}

	uint64_t CallGuestCallback(uint64_t guest_func_ptr, uint64_t arg0 = 0,
	                           uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0,
	                           uint64_t arg4 = 0, uint64_t arg5 = 0) override {
		if (guest_func_ptr == 0) {
			return 0;
		}

#if defined(__aarch64__)
		auto* thread = s_current_fex_thread;
		if (thread != nullptr && thread->CurrentFrame != nullptr) {
			auto* frame = thread->CurrentFrame;
			uint64_t saved_rdi = frame->State.gregs[FEXCore::X86State::REG_RDI];
			uint64_t saved_rsi = frame->State.gregs[FEXCore::X86State::REG_RSI];
			uint64_t saved_rdx = frame->State.gregs[FEXCore::X86State::REG_RDX];
			uint64_t saved_rcx = frame->State.gregs[FEXCore::X86State::REG_RCX];
			uint64_t saved_r8  = frame->State.gregs[FEXCore::X86State::REG_R8];
			uint64_t saved_r9  = frame->State.gregs[FEXCore::X86State::REG_R9];

			frame->State.gregs[FEXCore::X86State::REG_RDI] = arg0;
			frame->State.gregs[FEXCore::X86State::REG_RSI] = arg1;
			frame->State.gregs[FEXCore::X86State::REG_RDX] = arg2;
			frame->State.gregs[FEXCore::X86State::REG_RCX] = arg3;
			frame->State.gregs[FEXCore::X86State::REG_R8]  = arg4;
			frame->State.gregs[FEXCore::X86State::REG_R9]  = arg5;

			EnsureTrampolinePool();
			frame->Pointers.ThunkCallbackRet = reinterpret_cast<uint64_t>(s_trampoline_pool + 16);

			ALOGI("Arm64CpuBackend: Re-entrant JIT callback at 0x%016llx (arg0=0x%llx, arg1=0x%llx)",
			      static_cast<unsigned long long>(guest_func_ptr),
			      static_cast<unsigned long long>(arg0),
			      static_cast<unsigned long long>(arg1));

			m_fex_context->HandleCallback(thread, guest_func_ptr);

			uint64_t ret = frame->State.gregs[FEXCore::X86State::REG_RAX];
			ALOGI("Arm64CpuBackend: Re-entrant JIT callback 0x%016llx returned 0x%llx",
			      static_cast<unsigned long long>(guest_func_ptr),
			      static_cast<unsigned long long>(ret));

			frame->State.gregs[FEXCore::X86State::REG_RDI] = saved_rdi;
			frame->State.gregs[FEXCore::X86State::REG_RSI] = saved_rsi;
			frame->State.gregs[FEXCore::X86State::REG_RDX] = saved_rdx;
			frame->State.gregs[FEXCore::X86State::REG_RCX] = saved_rcx;
			frame->State.gregs[FEXCore::X86State::REG_R8]  = saved_r8;
			frame->State.gregs[FEXCore::X86State::REG_R9]  = saved_r9;

			return ret;
		}
#endif

		ALOGI("Arm64CpuBackend: Fallback ExecuteGuestCode callback at 0x%016llx",
		      static_cast<unsigned long long>(guest_func_ptr));
		return ExecuteGuestCode(guest_func_ptr, arg0, arg1, arg2, 0, arg3, arg4, arg5);
	}

private:
	uint64_t m_guest_base = 0;
	uint64_t m_guest_size = 0;
	CapySyscallHandler m_syscall_handler;
	CapySignalDelegator m_signal_delegator;
	fextl::unique_ptr<FEXCore::Context::Context> m_fex_context;

	uint64_t ExecuteGuestCode(uint64_t entry_addr, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t stack_top,
	                          uint64_t arg3 = 0, uint64_t arg4 = 0, uint64_t arg5 = 0) {
		if (!m_fex_context) {
			ALOGI("ExecuteGuestCode: Lazy initializing FEXCore context");
			if (!Initialize(0, 0x0000800000000000ULL)) {
				ALOGE("ExecuteGuestCode: Failed to initialize FEXCore context!");
				return 0;
			}
		}

		ALOGI("ExecuteGuestCode: 0. InitThread");
		FEXCore::Allocator::InitializeThread();

		ALOGI("ExecuteGuestCode: 1. EnsureTrampolinePool");
		EnsureTrampolinePool();
		uint64_t sentinel_addr = reinterpret_cast<uint64_t>(s_trampoline_pool);

		ALOGI("ExecuteGuestCode: 2. stack_top check (in=%016llx)", static_cast<unsigned long long>(stack_top));
#if defined(__aarch64__)
		// Nested run (an HLE call made by guest code, e.g. sceKernelLoadStartModule running
		// module_start): continue below the caller's live stack. Starting from the top of the
		// main guest stack overwrote the caller's frames (Unity games crashed on startup).
		if (stack_top == 0 && s_current_fex_thread != nullptr) {
			const auto caller_rsp =
			    s_current_fex_thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP];
			if (caller_rsp != 0) {
				stack_top = (caller_rsp - 0x400) & ~0xFull;
				ALOGI("ExecuteGuestCode: nested run below caller rsp=%016llx",
				      static_cast<unsigned long long>(caller_rsp));
			}
		}
#endif
		if (stack_top == 0) {
			void* main_stack = Libs::LibKernel::PthreadCreateMainGuestStack();
			stack_top = reinterpret_cast<uint64_t>(main_stack);
			ALOGI("ExecuteGuestCode: 2a. PthreadCreateMainGuestStack returned %016llx", static_cast<unsigned long long>(stack_top));
		}
		if (stack_top == 0) {
			static thread_local std::vector<uint8_t> s_fallback_stack(2 * 1024 * 1024);
			stack_top = reinterpret_cast<uint64_t>(s_fallback_stack.data() + s_fallback_stack.size());
			ALOGI("ExecuteGuestCode: 2b. fallback stack used: %016llx", static_cast<unsigned long long>(stack_top));
		}

		// Ensure 16-byte stack alignment
		uint64_t rsp = (stack_top - 0x80) & ~0x0Full;

		// Push HLT sentinel return address so ret terminates execution back to host
		rsp -= 8;
		ALOGI("ExecuteGuestCode: 3. writing sentinel to rsp=%016llx (sentinel=%016llx)",
		      static_cast<unsigned long long>(rsp), static_cast<unsigned long long>(sentinel_addr));
		*reinterpret_cast<uint64_t*>(rsp) = sentinel_addr;
		ALOGI("ExecuteGuestCode: 3a. sentinel written OK");

		static FEXCore::Core::CPUState::gdt_segment s_gdt[32] = {};
		static bool s_gdt_init = false;
		if (!s_gdt_init) {
			auto& cs_desc = s_gdt[FEXCore::Core::CPUState::DEFAULT_USER_CS];
			FEXCore::Core::CPUState::SetGDTBase(&cs_desc, 0);
			FEXCore::Core::CPUState::SetGDTLimit(&cs_desc, 0xFFFFF);
			cs_desc.L = 1; // 64-bit Long Mode
			cs_desc.D = 0; // Default operand size reserved for 64-bit
			s_gdt_init = true;
		}

		constexpr size_t CALLRET_PAGE_SIZE = 4096;
		constexpr size_t CALLRET_STACK_SIZE = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE; // 4MB
		constexpr size_t CALLRET_ALLOC_SIZE = CALLRET_STACK_SIZE + 2 * CALLRET_PAGE_SIZE;

		void* callret_alloc = mmap(nullptr, CALLRET_ALLOC_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		void* callret_base = nullptr;
		uint64_t callret_sp = 0;
		if (callret_alloc != MAP_FAILED && callret_alloc != nullptr) {
			callret_base = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(callret_alloc) + CALLRET_PAGE_SIZE);
			mprotect(callret_base, CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE);
			callret_sp = reinterpret_cast<uint64_t>(callret_base) + (CALLRET_STACK_SIZE / 2);
		} else {
			ALOGE("Arm64CpuBackend: Failed to allocate call-ret stack!");
		}

		FEXCore::Core::CPUState initial_state {};
		initial_state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &s_gdt[0];
		initial_state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &s_gdt[0];
		initial_state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
		initial_state.cs_cached = 0;
		initial_state.pf_raw = 1;
		initial_state.mxcsr = 0x1f80;
		initial_state.callret_sp = callret_sp;
		initial_state.rip = entry_addr;
		initial_state.gregs[FEXCore::X86State::REG_RSP] = rsp;
		initial_state.gregs[FEXCore::X86State::REG_RBP] = rsp;
		initial_state.gregs[FEXCore::X86State::REG_RDI] = arg0;
		initial_state.gregs[FEXCore::X86State::REG_RSI] = arg1;
		initial_state.gregs[FEXCore::X86State::REG_RDX] = arg2;
		initial_state.gregs[FEXCore::X86State::REG_RCX] = arg3;
		initial_state.gregs[FEXCore::X86State::REG_R8]  = arg4;
		initial_state.gregs[FEXCore::X86State::REG_R9]  = arg5;

		// Setup TCB base for fs:[offset] TLS accesses
		ALOGI("ExecuteGuestCode: 4. GetCurrentTcb");
		uint8_t* tcb = Loader::RuntimeLinker::GetCurrentTcb();
		if (tcb != nullptr) {
			initial_state.fs_cached = reinterpret_cast<uint64_t>(tcb);
		} else {
			static uint8_t s_fallback_tcb[65536] = {};
			initial_state.fs_cached = reinterpret_cast<uint64_t>(s_fallback_tcb);
		}
		ALOGI("ExecuteGuestCode: 4a. TCB fs_cached=%016llx", static_cast<unsigned long long>(initial_state.fs_cached));

		// Diagnose FEX thread allocation failures (Unity job workers crashed in CreateThread with
		// a null allocation).
		if (void* probe = FEXCore::Allocator::aligned_alloc(0x1000, 0x3000); probe == nullptr) {
			char name[32] = {};
			pthread_getname_np(pthread_self(), name, sizeof(name));
			ALOGE("ExecuteGuestCode: FEX allocator returned null on thread '%s' (tid %d, rpmalloc init=%d, errno=%d)",
			      name, gettid(), rpmalloc_is_thread_initialized(), errno);
			FEXCore::Allocator::InitializeThread();
		} else {
			FEXCore::Allocator::aligned_free(probe);
		}
		ALOGI("ExecuteGuestCode: 5. CreateThread(&initial_state)");
		auto* thread = m_fex_context->CreateThread(&initial_state);
		ALOGI("ExecuteGuestCode: 5a. CreateThread returned %p", static_cast<void*>(thread));
		if (!thread) {
			ALOGE("Arm64CpuBackend: Failed to create guest thread for entry 0x%016llx",
			      static_cast<unsigned long long>(entry_addr));
			if (callret_alloc && callret_alloc != MAP_FAILED) {
				munmap(callret_alloc, CALLRET_ALLOC_SIZE);
			}
			return 0;
		}

		thread->CallRetStackBase = callret_base;
		thread->CurrentFrame->Pointers.ThunkCallbackRet = reinterpret_cast<uint64_t>(s_trampoline_pool + 16);

		const uint8_t* code_bytes = reinterpret_cast<const uint8_t*>(entry_addr);
		ALOGI("Arm64CpuBackend: Guest entry code bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		      code_bytes[0], code_bytes[1], code_bytes[2], code_bytes[3],
		      code_bytes[4], code_bytes[5], code_bytes[6], code_bytes[7],
		      code_bytes[8], code_bytes[9], code_bytes[10], code_bytes[11],
		      code_bytes[12], code_bytes[13], code_bytes[14], code_bytes[15]);

		ALOGI("Arm64CpuBackend: JIT entering guest code at RIP=0x%016llx, RSP=0x%016llx, RDI=0x%016llx, RSI=0x%016llx, RDX=0x%016llx",
		      static_cast<unsigned long long>(entry_addr), static_cast<unsigned long long>(rsp),
		      static_cast<unsigned long long>(arg0), static_cast<unsigned long long>(arg1), static_cast<unsigned long long>(arg2));

		CaptureHleCallerState();
		s_nested_guest_runs++;
#if defined(__aarch64__)
		auto* const outer_fex_thread = s_current_fex_thread;
		s_current_fex_thread = thread;
#endif
		m_fex_context->ExecuteThread(thread);
#if defined(__aarch64__)
		s_current_fex_thread = outer_fex_thread;
#endif

		uint64_t ret = thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
		ALOGI("Arm64CpuBackend: JIT returned from guest code with RAX=0x%016llx", static_cast<unsigned long long>(ret));
		{
			// A normal exit pops the sentinel: RSP = entry RSP + 8 and RIP just past the HLT.
			const auto end_rsp = thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP];
			const auto end_rip = thread->CurrentFrame->State.rip;
			if (end_rsp != rsp + 8) {
				__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Cpu",
				    "guest run 0x%llx ended abnormally: rip=0x%llx rsp=0x%llx (entry rsp=0x%llx) rax=0x%llx",
				    static_cast<unsigned long long>(entry_addr), static_cast<unsigned long long>(end_rip),
				    static_cast<unsigned long long>(end_rsp), static_cast<unsigned long long>(rsp),
				    static_cast<unsigned long long>(ret));
			}
		}

		m_fex_context->DestroyThread(thread);
		if (callret_alloc && callret_alloc != MAP_FAILED) {
			munmap(callret_alloc, CALLRET_ALLOC_SIZE);
		}
		return ret;
	}
};

#endif

bool GetCurrentGuestRegisters(GuestRegisters* regs) {
#if defined(__aarch64__)
	auto* thread = s_current_fex_thread;
	if (regs == nullptr || thread == nullptr || thread->CurrentFrame == nullptr) {
		return false;
	}
	const auto& state = thread->CurrentFrame->State;
	regs->rax     = state.gregs[FEXCore::X86State::REG_RAX];
	regs->rbx     = state.gregs[FEXCore::X86State::REG_RBX];
	regs->rcx     = state.gregs[FEXCore::X86State::REG_RCX];
	regs->rdx     = state.gregs[FEXCore::X86State::REG_RDX];
	regs->rsi     = state.gregs[FEXCore::X86State::REG_RSI];
	regs->rdi     = state.gregs[FEXCore::X86State::REG_RDI];
	regs->rbp     = state.gregs[FEXCore::X86State::REG_RBP];
	regs->rsp     = state.gregs[FEXCore::X86State::REG_RSP];
	regs->r8      = state.gregs[FEXCore::X86State::REG_R8];
	regs->r9      = state.gregs[FEXCore::X86State::REG_R9];
	regs->r10     = state.gregs[FEXCore::X86State::REG_R10];
	regs->r11     = state.gregs[FEXCore::X86State::REG_R11];
	regs->r12     = state.gregs[FEXCore::X86State::REG_R12];
	regs->r13     = state.gregs[FEXCore::X86State::REG_R13];
	regs->r14     = state.gregs[FEXCore::X86State::REG_R14];
	regs->r15     = state.gregs[FEXCore::X86State::REG_R15];
	regs->rip     = state.rip;
	regs->rflags  = 0x202;
	regs->fs_base = state.fs_cached;
	return true;
#else
	(void)regs;
	return false;
#endif
}

bool HandleFexUnalignedAccess(void* native_context) {
#if defined(__aarch64__)
	auto* context = static_cast<ucontext_t*>(native_context);
	auto* thread = s_current_fex_thread;
	if (!thread || !context) {
		return false;
	}
	uintptr_t pc = static_cast<uintptr_t>(context->uc_mcontext.pc);
	uint64_t* gprs = reinterpret_cast<uint64_t*>(context->uc_mcontext.regs);
	const auto result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
		thread,
		FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic,
		pc,
		gprs,
		true
	);
	if (result.has_value()) {
		context->uc_mcontext.pc = static_cast<uint64_t>(pc + result.value());
		ALOGI("CpuBackend: Handled unaligned access at PC=0x%llx, delta=%d, new_PC=0x%llx",
		      static_cast<unsigned long long>(pc), result.value(),
		      static_cast<unsigned long long>(context->uc_mcontext.pc));
		return true;
	}
#else
	(void)native_context;
#endif
	return false;
}

CpuBackend* CpuBackend::Instance() {
#if defined(__x86_64__) || defined(_M_X64)
	static NativeCpuBackend instance;
	return &instance;
#else
	static Arm64CpuBackend instance;
	return &instance;
#endif
}

} // namespace Cpu

namespace Cpu {
uint64_t LastHleFunction() {
	return t_last_hle_func;
}

uint64_t GuestReturnAddress() {
#if defined(__aarch64__)
	// Inside an HLE call the guest stack starts with the caller's return address.
	if (s_current_fex_thread != nullptr && s_current_fex_thread->CurrentFrame != nullptr) {
		const auto rsp = s_current_fex_thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP];
		uint64_t   value = 0;
		if (rsp != 0 && Libs::LibKernel::Memory::ReadGuestMemory(rsp, &value, sizeof(value))) {
			return value;
		}
	}
#endif
	return 0;
}

uint64_t GuestRipFromHostPc(uint64_t host_pc) {
#if defined(__aarch64__)
	auto* thread = s_current_fex_thread;
	if (thread != nullptr && thread->CurrentFrame != nullptr && thread->CTX != nullptr) {
		return thread->CTX->RestoreRIPFromHostPC(thread, host_pc);
	}
#else
	(void)host_pc;
#endif
	return 0;
}
} // namespace Cpu
