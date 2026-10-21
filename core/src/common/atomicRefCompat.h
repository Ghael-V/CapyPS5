// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef KYTY_COMMON_ATOMIC_REF_COMPAT_H_
#define KYTY_COMMON_ATOMIC_REF_COMPAT_H_

#include <atomic>

#if !defined(__cpp_lib_atomic_ref) && !defined(FEX_ATOMIC_REF_POLYFILL_DEFINED)
#define FEX_ATOMIC_REF_POLYFILL_DEFINED 1
namespace std {
template <typename T>
struct atomic_ref {
	T* ptr;
	explicit atomic_ref(T& obj) noexcept : ptr(&obj) {}
	T load(memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_load_n(ptr, static_cast<int>(order));
	}
	void store(T val, memory_order order = memory_order_seq_cst) noexcept {
		__atomic_store_n(ptr, val, static_cast<int>(order));
	}
	T fetch_add(T arg, memory_order order = memory_order_seq_cst) noexcept {
		return __atomic_fetch_add(ptr, arg, static_cast<int>(order));
	}
	T fetch_sub(T arg, memory_order order = memory_order_seq_cst) noexcept {
		return __atomic_fetch_sub(ptr, arg, static_cast<int>(order));
	}
	T fetch_and(T arg, memory_order order = memory_order_seq_cst) noexcept {
		return __atomic_fetch_and(ptr, arg, static_cast<int>(order));
	}
	T fetch_or(T arg, memory_order order = memory_order_seq_cst) noexcept {
		return __atomic_fetch_or(ptr, arg, static_cast<int>(order));
	}
	T fetch_xor(T arg, memory_order order = memory_order_seq_cst) noexcept {
		return __atomic_fetch_xor(ptr, arg, static_cast<int>(order));
	}
	bool compare_exchange_strong(T& expected, T desired,
	                              memory_order success, memory_order failure) noexcept {
		return __atomic_compare_exchange_n(ptr, &expected, desired, false,
		                                  static_cast<int>(success), static_cast<int>(failure));
	}
	bool compare_exchange_strong(T& expected, T desired,
	                              memory_order order = memory_order_seq_cst) noexcept {
		return compare_exchange_strong(expected, desired, order, order);
	}
};

template <typename T>
atomic_ref(T&) -> atomic_ref<T>;
} // namespace std
#endif

#endif // KYTY_COMMON_ATOMIC_REF_COMPAT_H_
