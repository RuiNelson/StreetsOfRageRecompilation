#pragma once

// The SDK's libc++ 18 has no std::atomic_ref (it came in libc++ 19). Force-included in the
// PS5 build (cmake/ps5-sor.cmake), for the parts MegaDriveEnvironment uses, on a target
// where these widths are lock-free.
#include <atomic>
#include <cstddef>
#include <type_traits>

_LIBCPP_BEGIN_NAMESPACE_STD

template <class T>
class atomic_ref {
    static_assert(is_trivially_copyable_v<T>);

public:
    static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), 0);

    explicit atomic_ref(T &object) noexcept : pointer_(std::addressof(object)) {}

    void store(T value, memory_order order = memory_order_seq_cst) const noexcept {
        __atomic_store_n(pointer_, value, static_cast<int>(order));
    }
    T load(memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_load_n(pointer_, static_cast<int>(order));
    }
    T exchange(T value, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_exchange_n(pointer_, value, static_cast<int>(order));
    }
    T fetch_add(T value, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_fetch_add(pointer_, value, static_cast<int>(order));
    }
    T fetch_sub(T value, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_fetch_sub(pointer_, value, static_cast<int>(order));
    }
    T fetch_or(T value, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_fetch_or(pointer_, value, static_cast<int>(order));
    }
    T fetch_and(T value, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_fetch_and(pointer_, value, static_cast<int>(order));
    }

private:
    T *pointer_;
};

_LIBCPP_END_NAMESPACE_STD
