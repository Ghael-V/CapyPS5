// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef CPU_HOST_CPU_FEATURES_H
#define CPU_HOST_CPU_FEATURES_H

#if defined(__aarch64__) && defined(__ANDROID__)

namespace FEXCore {
struct HostFeatures;
} // namespace FEXCore

namespace Cpu {

// Fills the FEX host features from what this ARM64 CPU really supports (the kernel's HWCAP bits and
// the cores' MIDR), the way FEX's own frontend does, so the JIT can use FlagM, FRINTTS, LRCPC2, AFP,
// RPRES, CSSC and the rest. Features that change what the guest sees in CPUID are only enabled when
// the PS5 CPU (Zen 2) has them too. Logs the result (tag CapyPS5-Cpu).
// `setprop debug.capy.fex_host_features 0` keeps the old fixed list, for A/B comparisons.
void DetectFexHostFeatures(FEXCore::HostFeatures& features);

} // namespace Cpu

#endif

#endif // CPU_HOST_CPU_FEATURES_H
