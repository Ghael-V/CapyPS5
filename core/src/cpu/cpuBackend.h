// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_CPU_CPUBACKEND_H_
#define EMULATOR_SRC_CPU_CPUBACKEND_H_

#include <type_traits>
#include "common/abi.h"
#include "common/common.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Loader {
struct EntryParams;
using atexit_func_t = KYTY_SYSV_ABI void (*)();
using module_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp);
} // namespace Loader

namespace Libs::LibKernel {
using pthread_entry_func_t = KYTY_SYSV_ABI void* (*)(void*);
} // namespace Libs::LibKernel

namespace Cpu {

/** Host address of the last HLE function the current thread returned from (diagnostics). */
uint64_t LastHleFunction();
/** Guest address an HLE function returns to (0 when unknown; diagnostics). */
uint64_t GuestReturnAddress();
/** Guest RIP of the translated code at host address `host_pc` on this thread (0 when unknown). */
uint64_t GuestRipFromHostPc(uint64_t host_pc);

enum class BackendType {
	Native,      // Direct x86_64 host execution (PC)
	Dynarec,     // Dynamic binary translation (FEXCore / ARM64 JIT)
	Interpreter  // Fallback bytecode interpreter
};

class CpuBackend {
public:
	virtual ~CpuBackend() = default;

	[[nodiscard]] virtual BackendType GetType() const = 0;
	[[nodiscard]] virtual const char* GetName() const = 0;

	virtual bool Initialize(uint64_t guest_base, uint64_t guest_size) = 0;
	virtual void Shutdown() = 0;

	// Execute ELF entry point
	virtual void RunEntry(uint64_t addr, Loader::EntryParams* params,
	                      Loader::atexit_func_t atexit_func, void* stack_top) = 0;

	// Execute PRX module initialization / finalization
	virtual int RunModuleInit(uint64_t addr, size_t args, const void* argp,
	                          Loader::module_func_t func) = 0;
	virtual int RunModuleFini(uint64_t addr, size_t args, const void* argp,
	                          Loader::module_func_t func) = 0;

	// Execute guest thread worker on guest stack
	virtual void* RunOnGuestStack(void* arg, Libs::LibKernel::pthread_entry_func_t func,
	                              void* stack_top) = 0;

	// Execute guest callback on current JIT thread or dedicated stack
	virtual uint64_t CallGuestCallback(uint64_t guest_func_ptr, uint64_t arg0 = 0,
	                                   uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0,
	                                   uint64_t arg4 = 0, uint64_t arg5 = 0) = 0;

	// Singleton instance
	static CpuBackend* Instance();
};

namespace Detail {
template <typename T>
uint64_t ToGuestArg(T value) {
	if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
		return reinterpret_cast<uint64_t>(value);
	} else {
		return static_cast<uint64_t>(value);
	}
}
} // namespace Detail

/**
 * Calls a guest function pointer (a callback the game handed to an HLE function): through the JIT
 * on ARM64 hosts, natively on x86-64. Calling one directly from host code jumps into x86 code and
 * dies with SIGILL on ARM64, so every guest callback must go through here. Integer and pointer
 * arguments only, up to six (RDI, RSI, RDX, RCX, R8, R9).
 */
template <typename Ret = uint64_t, typename Func, typename... Args>
Ret CallGuest(Func func, Args... args) {
	static_assert(sizeof...(Args) <= 6, "guest callbacks take at most six register arguments");
	const auto result = CpuBackend::Instance()->CallGuestCallback(
	    reinterpret_cast<uint64_t>(func), Detail::ToGuestArg(args)...);
	if constexpr (std::is_void_v<Ret>) {
		(void)result;
	} else if constexpr (std::is_pointer_v<Ret>) {
		return reinterpret_cast<Ret>(result);
	} else {
		return static_cast<Ret>(result);
	}
}

// Create a guest-callable trampoline that forwards an x86_64 call to an ARM64 host function
uint64_t GetOrCreateHleTrampoline(uint64_t host_func_ptr, const char* name = nullptr);

// Guest-native replacement for an HLE function: when the trampoline of `host_func_ptr` is created,
// the guest gets the x86 code `factory` returns instead of a trap, so hot calls that need no host
// state (a counter read, a TLS base) skip the round trip. The factory runs once, on first use.
using GuestNativeFactory = std::vector<uint8_t> (*)();
void RegisterGuestNative(uint64_t host_func_ptr, GuestNativeFactory factory);
// Copies x86 code to executable guest memory and returns its address (never freed).
uint64_t AllocateGuestCode(const std::vector<uint8_t>& code);

// x86 registers of the guest thread running on this host thread, as of its current HLE call.
struct GuestRegisters {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, fs_base;
};
bool GetCurrentGuestRegisters(GuestRegisters* regs);

// Handle unaligned memory access triggered by FEXCore JIT on ARM64 host
bool HandleFexUnalignedAccess(void* native_context);

} // namespace Cpu

#endif // EMULATOR_SRC_CPU_CPUBACKEND_H_
