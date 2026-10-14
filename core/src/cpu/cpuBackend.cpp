#include "cpu/cpuBackend.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/threads.h"
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
#include <FEXCore/fextl/memory.h>
#if defined(__aarch64__)
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <ucontext.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace FEXCore::Allocator {
void InitializeAllocator(size_t PageSize);
void InitializeThread();
}
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
	                           uint64_t arg1 = 0, uint64_t arg2 = 0) override {
		using callback_fn_t = KYTY_SYSV_ABI uint64_t (*)(uint64_t, uint64_t, uint64_t);
		return reinterpret_cast<callback_fn_t>(guest_func_ptr)(arg0, arg1, arg2);
	}
};

#else // Architecture is ARM64 or other non-x86_64 host

namespace {

#if defined(__aarch64__)
thread_local FEXCore::Core::InternalThreadState* s_current_fex_thread = nullptr;
#endif

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

			using hle_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t,
			                             uint64_t, uint64_t, uint64_t, uint64_t);
			auto fn = reinterpret_cast<hle_fn_t>(host_func);
			uint64_t ret = fn(a0, a1, a2, a3, a4, a5, a6, a7,
			                  a8, a9, a10, a11, a12, a13, a14, a15);

			Frame->State.gregs[FEXCore::X86State::REG_RAX] = ret;
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

		FEXCore::HostFeatures features {};
		features.DCacheLineLog2 = 6;
		features.SupportsCacheMaintenanceOps = 1;
		features.SupportsAES = 1;
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
	                           uint64_t arg1 = 0, uint64_t arg2 = 0) override {
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

			frame->State.gregs[FEXCore::X86State::REG_RDI] = arg0;
			frame->State.gregs[FEXCore::X86State::REG_RSI] = arg1;
			frame->State.gregs[FEXCore::X86State::REG_RDX] = arg2;

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

			return ret;
		}
#endif

		ALOGI("Arm64CpuBackend: Fallback ExecuteGuestCode callback at 0x%016llx",
		      static_cast<unsigned long long>(guest_func_ptr));
		return ExecuteGuestCode(guest_func_ptr, arg0, arg1, arg2, 0);
	}

private:
	uint64_t m_guest_base = 0;
	uint64_t m_guest_size = 0;
	CapySyscallHandler m_syscall_handler;
	CapySignalDelegator m_signal_delegator;
	fextl::unique_ptr<FEXCore::Context::Context> m_fex_context;

	uint64_t ExecuteGuestCode(uint64_t entry_addr, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t stack_top) {
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

#if defined(__aarch64__)
		s_current_fex_thread = thread;
#endif
		m_fex_context->ExecuteThread(thread);
#if defined(__aarch64__)
		s_current_fex_thread = nullptr;
#endif

		uint64_t ret = thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
		ALOGI("Arm64CpuBackend: JIT returned from guest code with RAX=0x%016llx", static_cast<unsigned long long>(ret));

		m_fex_context->DestroyThread(thread);
		if (callret_alloc && callret_alloc != MAP_FAILED) {
			munmap(callret_alloc, CALLRET_ALLOC_SIZE);
		}
		return ret;
	}
};

#endif

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
