#ifndef EMULATOR_SRC_CPU_CPUBACKEND_H_
#define EMULATOR_SRC_CPU_CPUBACKEND_H_

#include "common/abi.h"
#include "common/common.h"

#include <cstddef>
#include <cstdint>

namespace Loader {
struct EntryParams;
using atexit_func_t = KYTY_SYSV_ABI void (*)();
using module_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp);
} // namespace Loader

namespace Libs::LibKernel {
using pthread_entry_func_t = KYTY_SYSV_ABI void* (*)(void*);
} // namespace Libs::LibKernel

namespace Cpu {

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
	                                   uint64_t arg1 = 0, uint64_t arg2 = 0) = 0;

	// Singleton instance
	static CpuBackend* Instance();
};

// Create a guest-callable trampoline that forwards an x86_64 call to an ARM64 host function
uint64_t GetOrCreateHleTrampoline(uint64_t host_func_ptr, const char* name = nullptr);

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
