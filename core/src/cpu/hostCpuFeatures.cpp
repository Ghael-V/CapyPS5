// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only
//
// The detection follows FEX's frontend (Source/Common/HostFeatures.cpp, MIT), which CapyPS5 does not
// build: it reads the ID registers, here the kernel's HWCAP bits give the same answers.

#include "cpu/hostCpuFeatures.h"

#if defined(__aarch64__) && defined(__ANDROID__)

#include "common/debugFlags.h"

#include <FEXCore/Core/HostFeatures.h>

#include <android/log.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Cpu {

namespace {

// arch/arm64/include/uapi/asm/hwcap.h
constexpr uint64_t kHwcapPmull       = 1ULL << 4;
constexpr uint64_t kHwcapAtomics     = 1ULL << 8;
constexpr uint64_t kHwcapFcma        = 1ULL << 14;
constexpr uint64_t kHwcapLrcpc       = 1ULL << 15;
constexpr uint64_t kHwcapIlrcpc      = 1ULL << 26; // FEAT_LRCPC2
constexpr uint64_t kHwcapFlagm       = 1ULL << 27;
constexpr uint64_t kHwcap2Sve2       = 1ULL << 1;
constexpr uint64_t kHwcap2SveBitperm = 1ULL << 4;
constexpr uint64_t kHwcap2Flagm2     = 1ULL << 7;
constexpr uint64_t kHwcap2Frint      = 1ULL << 8;
constexpr uint64_t kHwcap2Ecv        = 1ULL << 19;
constexpr uint64_t kHwcap2Afp        = 1ULL << 20;
constexpr uint64_t kHwcap2Rpres      = 1ULL << 21;
constexpr uint64_t kHwcap2Cssc       = 1ULL << 34;
constexpr uint64_t kHwcap2Mops       = 1ULL << 43;

uint32_t ReadMidr(long cpu) {
	char path[96];
	std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", cpu);
	FILE* file = std::fopen(path, "r");
	if (file == nullptr) {
		return 0;
	}
	char text[32] {};
	const bool ok = std::fgets(text, sizeof(text), file) != nullptr;
	std::fclose(file);
	return ok ? static_cast<uint32_t>(std::strtoull(text, nullptr, 16)) : 0;
}

// Cortex cores whose LDAPUR (the LRCPC2 load with an unscaled offset) has full acquire ordering
// and is slow (Arm erratum 3877900, the list LLVM uses).
bool HasSlowLdapur(uint32_t midr) {
	const uint32_t implementer = (midr >> 24) & 0xff;
	const uint32_t part        = (midr >> 4) & 0xfff;
	if (implementer != 0x41) {
		return false;
	}
	switch (part) {
		case 0xd4f: // Neoverse V2
		case 0xd84: // Neoverse V3
		case 0xd83: // Neoverse V3AE
		case 0xd4e: // Cortex-X3
		case 0xd82: // Cortex-X4
		case 0xd85: // Cortex-X925
		case 0xd8c: // C1-Ultra
		case 0xd90: // C1-Premium
			return true;
		default: return false;
	}
}

void SetFixedFeatures(FEXCore::HostFeatures& features) {
	features.DCacheLineLog2              = 6;
	features.SupportsCacheMaintenanceOps = 1;
	features.SupportsAtomics             = 1;
	features.SupportsRCPC                = 1;
}

} // namespace

void DetectFexHostFeatures(FEXCore::HostFeatures& features) {
	// What the guest sees in CPUID, kept as before: PS5 games use AES, SHA-NI (Guri: sha1msg1) and
	// CRC32 without checking, and FEX translates them to the ARMv8 crypto instructions every
	// supported Snapdragon has. AVX: the PS5 CPU has it (FEX runs it as two 128-bit halves).
	features.SupportsAES = 1;
	features.SupportsSHA = 1;
	features.SupportsCRC = 1;
	features.SupportsAVX = 1;

	static Common::DebugFlags::CachedValue detect("debug.capy.fex_host_features", 1);
	if (detect.Get() == 0) {
		SetFixedFeatures(features);
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Cpu", "FEX host features: fixed list (debug.capy.fex_host_features 0)");
		return;
	}

	const uint64_t hwcap  = getauxval(AT_HWCAP);
	const uint64_t hwcap2 = getauxval(AT_HWCAP2);

	// CTR_EL0 and DCZID_EL0 are readable at EL0.
	uint64_t ctr   = 0;
	uint64_t dczid = 0;
	__asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	__asm volatile("mrs %0, dczid_el0" : "=r"(dczid));
	// Smallest data cache line, as log2 of 4-byte words (FEXCore's DCacheSize() is 4 << this).
	features.DCacheLineLog2              = (ctr >> 16) & 0xf;
	features.SupportsCacheMaintenanceOps = 1;

	// JIT-only: these change the ARM64 code FEX emits, not what the guest sees.
	features.SupportsAtomics    = (hwcap & kHwcapAtomics) != 0;
	features.SupportsRCPC       = (hwcap & kHwcapLrcpc) != 0;
	features.SupportsTSOImm9    = (hwcap & kHwcapIlrcpc) != 0;
	features.SupportsFCMA       = (hwcap & kHwcapFcma) != 0;
	features.SupportsFlagM      = (hwcap & kHwcapFlagm) != 0;
	features.SupportsFlagM2     = (hwcap2 & kHwcap2Flagm2) != 0;
	features.SupportsFRINTTS    = (hwcap2 & kHwcap2Frint) != 0;
	features.SupportsECV        = (hwcap2 & kHwcap2Ecv) != 0;
	features.SupportsRPRES      = (hwcap2 & kHwcap2Rpres) != 0;
	features.SupportsCSSC       = (hwcap2 & kHwcap2Cssc) != 0;
	features.SupportsMOPS       = (hwcap2 & kHwcap2Mops) != 0;
	features.SupportsSVEBitPerm = (hwcap2 & kHwcap2SveBitperm) != 0;
	// FEAT_AFP: lets FEX keep the upper lanes of scalar SSE results in hardware (FPCR.NEP) and
	// apply MXCSR.DAZ through FPCR.FIZ, as the console's FTZ+DAZ start state expects.
	features.SupportsAFP = (hwcap2 & kHwcap2Afp) != 0;
	// FEX only uses SVE when SVE2 is there; 256-bit vectors run AVX natively.
	features.SupportsSVE128 = (hwcap2 & kHwcap2Sve2) != 0;
	if (features.SupportsSVE128) {
		const int vl = prctl(PR_SVE_GET_VL);
		features.SupportsSVE256 = vl >= 0 && (vl & PR_SVE_VL_LEN_MASK) >= 32;
	}
#if defined(CAPY_FEX_PRESERVE_ALL) && CAPY_FEX_PRESERVE_ALL
	// FEXCore's fallback handlers were built with clang's preserve_all: calls to them only spill
	// the registers that ABI clobbers.
	features.SupportsPreserveAllABI = 1;
#endif

	// Guest-visible, and the PS5 CPU (Zen 2) has them: without host support FEX leaves PCLMULQDQ
	// and CLZERO unimplemented, so a game that uses them without checking CPUID would stop.
	features.SupportsPMULL_128Bit = (hwcap & kHwcapPmull) != 0;
	features.SupportsCLZERO       = (dczid & 0x10) == 0 && (4U << (dczid & 0xf)) == 64;
	// Left off, the PS5 CPU does not have them: WAITPKG (SupportsWFXT), AVX-VNNI (SupportsDotProd,
	// SupportsI8MM), 3DNow!, SSE4a. RDRAND stays off too: on Qualcomm Oryon RNDRRS never succeeds.

	// Any core with the slow LDAPUR makes the whole process avoid it (threads migrate).
	bool slow_ldapur = false;
	const long cpus  = sysconf(_SC_NPROCESSORS_CONF);
	for (long cpu = 0; cpu < cpus && !slow_ldapur; cpu++) {
		slow_ldapur = HasSlowLdapur(ReadMidr(cpu));
	}
	if (slow_ldapur) {
		features.SupportsTSOImm9 = 0;
	}

	// `setprop debug.capy.fex_feature_off <mask>` turns single features back off, to measure each:
	// 1 LRCPC2, 2 FCMA, 4 FlagM, 8 FlagM2, 0x10 FRINTTS, 0x20 ECV, 0x40 RPRES, 0x80 AFP,
	// 0x100 preserve_all, 0x200 PMULL128, 0x400 CLZERO, 0x800 the old 256-byte cache line,
	// 0x1000 CSSC, 0x2000 MOPS, 0x4000 SVE.
	static Common::DebugFlags::CachedValue off_mask("debug.capy.fex_feature_off", 0);
	const auto off = static_cast<uint64_t>(std::max<int64_t>(off_mask.Get(), 0));
	// FEX's fields are bit-fields: no references.
#define CAPY_CLEAR_FEATURE(bit, field) features.field = (off & (bit)) != 0 ? 0 : features.field;
	CAPY_CLEAR_FEATURE(0x1, SupportsTSOImm9)
	CAPY_CLEAR_FEATURE(0x2, SupportsFCMA)
	CAPY_CLEAR_FEATURE(0x4, SupportsFlagM)
	CAPY_CLEAR_FEATURE(0x8, SupportsFlagM2)
	CAPY_CLEAR_FEATURE(0x10, SupportsFRINTTS)
	CAPY_CLEAR_FEATURE(0x20, SupportsECV)
	CAPY_CLEAR_FEATURE(0x40, SupportsRPRES)
	CAPY_CLEAR_FEATURE(0x80, SupportsAFP)
	CAPY_CLEAR_FEATURE(0x100, SupportsPreserveAllABI)
	CAPY_CLEAR_FEATURE(0x200, SupportsPMULL_128Bit)
	CAPY_CLEAR_FEATURE(0x400, SupportsCLZERO)
	CAPY_CLEAR_FEATURE(0x1000, SupportsCSSC)
	CAPY_CLEAR_FEATURE(0x2000, SupportsMOPS)
	CAPY_CLEAR_FEATURE(0x4000, SupportsSVE128)
	CAPY_CLEAR_FEATURE(0x4000, SupportsSVE256)
	CAPY_CLEAR_FEATURE(0x4000, SupportsSVEBitPerm)
#undef CAPY_CLEAR_FEATURE
	if ((off & 0x800) != 0) {
		features.DCacheLineLog2 = 6;
	}

	std::string list;
	const auto add = [&list](bool on, const char* name) {
		if (on) {
			list += ' ';
			list += name;
		}
	};
	add(features.SupportsAtomics, "LSE");
	add(features.SupportsRCPC, "LRCPC");
	add(features.SupportsTSOImm9, "LRCPC2");
	add(features.SupportsFCMA, "FCMA");
	add(features.SupportsFlagM, "FlagM");
	add(features.SupportsFlagM2, "FlagM2");
	add(features.SupportsFRINTTS, "FRINTTS");
	add(features.SupportsECV, "ECV");
	add(features.SupportsRPRES, "RPRES");
	add(features.SupportsAFP, "AFP");
	add(features.SupportsCSSC, "CSSC");
	add(features.SupportsMOPS, "MOPS");
	add(features.SupportsSVE128, "SVE2");
	add(features.SupportsSVE256, "SVE256");
	add(features.SupportsSVEBitPerm, "SVE-BitPerm");
	add(features.SupportsPreserveAllABI, "preserve_all");
	add(features.SupportsPMULL_128Bit, "PMULL128");
	add(features.SupportsCLZERO, "CLZERO");
	__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Cpu", "FEX host features:%s; cache line %u B%s", list.c_str(),
	                    4U << features.DCacheLineLog2, slow_ldapur ? "; LRCPC2 off (slow LDAPUR core)" : "");
}

} // namespace Cpu

#endif
