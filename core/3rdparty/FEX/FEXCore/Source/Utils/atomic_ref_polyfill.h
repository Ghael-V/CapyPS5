// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <type_traits>

#if !defined(__cpp_lib_atomic_ref) && !defined(FEX_ATOMIC_REF_POLYFILL_DEFINED)
#define FEX_ATOMIC_REF_POLYFILL_DEFINED 1
namespace std {

template<typename T>
struct atomic_ref {
	static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
	T* ptr;

	explicit atomic_ref(T& obj) noexcept : ptr(&obj) {}
	atomic_ref(const atomic_ref&) noexcept = default;
	atomic_ref& operator=(const atomic_ref&) = delete;

	void store(T desired, memory_order order = memory_order_seq_cst) const noexcept {
		__atomic_store_n(ptr, desired, static_cast<int>(order));
	}

	T load(memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_load_n(ptr, static_cast<int>(order));
	}

	operator T() const noexcept { return load(); }

	T exchange(T desired, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_exchange_n(ptr, desired, static_cast<int>(order));
	}

	bool compare_exchange_weak(T& expected, T desired,
	                           memory_order success, memory_order failure) const noexcept {
		return __atomic_compare_exchange_n(ptr, &expected, desired, true,
		                                   static_cast<int>(success), static_cast<int>(failure));
	}

	bool compare_exchange_strong(T& expected, T desired,
	                             memory_order success, memory_order failure) const noexcept {
		return __atomic_compare_exchange_n(ptr, &expected, desired, false,
		                                   static_cast<int>(success), static_cast<int>(failure));
	}

	bool compare_exchange_strong(T& expected, T desired,
	                             memory_order order = memory_order_seq_cst) const noexcept {
		int s = static_cast<int>(order);
		int f = (order == memory_order_acq_rel) ? static_cast<int>(memory_order_acquire) :
		        (order == memory_order_release) ? static_cast<int>(memory_order_relaxed) : s;
		return __atomic_compare_exchange_n(ptr, &expected, desired, false, s, f);
	}

	bool compare_exchange_weak(T& expected, T desired,
	                           memory_order order = memory_order_seq_cst) const noexcept {
		int s = static_cast<int>(order);
		int f = (order == memory_order_acq_rel) ? static_cast<int>(memory_order_acquire) :
		        (order == memory_order_release) ? static_cast<int>(memory_order_relaxed) : s;
		return __atomic_compare_exchange_n(ptr, &expected, desired, true, s, f);
	}

	T fetch_add(T arg, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_fetch_add(ptr, arg, static_cast<int>(order));
	}

	T fetch_sub(T arg, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_fetch_sub(ptr, arg, static_cast<int>(order));
	}

	T fetch_and(T arg, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_fetch_and(ptr, arg, static_cast<int>(order));
	}

	T fetch_or(T arg, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_fetch_or(ptr, arg, static_cast<int>(order));
	}

	T fetch_xor(T arg, memory_order order = memory_order_seq_cst) const noexcept {
		return __atomic_fetch_xor(ptr, arg, static_cast<int>(order));
	}

	T operator++() const noexcept { return fetch_add(1) + 1; }
	T operator++(int) const noexcept { return fetch_add(1); }
	T operator--() const noexcept { return fetch_sub(1) - 1; }
	T operator--(int) const noexcept { return fetch_sub(1); }
};

} // namespace std
#endif
