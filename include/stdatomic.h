#ifndef _STDATOMIC_H
#define _STDATOMIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST,
} memory_order;

#define atomic_store_explicit(ptr, val, order) __atomic_store_n(ptr, val, order)
#define atomic_store(object, desired) \
    atomic_store_explicit(object, desired, __ATOMIC_SEQ_CST)

#define atomic_load_explicit(ptr, order) __atomic_load_n(ptr, order)
#define atomic_load(object) \
    atomic_load_explicit(object, __ATOMIC_SEQ_CST)

#define atomic_exchange_explicit(ptr, val, order) __atomic_exchange_n(ptr, val, order)
#define atomic_exchange(object, desired) \
    atomic_exchange_explicit(object, desired, __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_strong_explicit(ptr, expected, desired, s, f) \
    __atomic_compare_exchange_n(ptr, expected, desired, 0, s, f)
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit(object, expected, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(ptr, expected, desired, s, f) \
    __atomic_compare_exchange_n(ptr, expected, desired, 1, s, f)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit(object, expected, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define atomic_fetch_add_explicit(ptr, val, order) __atomic_fetch_add(ptr, val, order)
#define atomic_fetch_add(object, operand) \
    atomic_fetch_add_explicit(object, operand, __ATOMIC_SEQ_CST)

#define atomic_fetch_sub_explicit(ptr, val, order) __atomic_fetch_sub(ptr, val, order)
#define atomic_fetch_sub(object, operand) \
    atomic_fetch_sub_explicit(object, operand, __ATOMIC_SEQ_CST)

#define atomic_fetch_or_explicit(ptr, val, order) __atomic_fetch_or(ptr, val, order)
#define atomic_fetch_or(object, operand) \
    atomic_fetch_or_explicit(object, operand, __ATOMIC_SEQ_CST)

#define atomic_fetch_xor_explicit(ptr, val, order) __atomic_fetch_xor(ptr, val, order)
#define atomic_fetch_xor(object, operand) \
    atomic_fetch_xor_explicit(object, operand, __ATOMIC_SEQ_CST)

#define atomic_fetch_and_explicit(ptr, val, order) __atomic_fetch_and(ptr, val, order)
#define atomic_fetch_and(object, operand) \
    atomic_fetch_and_explicit(object, operand, __ATOMIC_SEQ_CST)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)
#define atomic_is_lock_free(obj) __atomic_is_lock_free(sizeof(*(obj)), obj)

#define atomic_flag_test_and_set_explicit(ptr, order) __atomic_test_and_set(ptr, order)
#define atomic_flag_test_and_set(ptr) \
    atomic_flag_test_and_set_explicit(ptr, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(ptr, order) __atomic_clear(ptr, order)
#define atomic_flag_clear(ptr) \
    atomic_flag_clear_explicit(ptr, __ATOMIC_SEQ_CST)

#define atomic_init(object, desired) atomic_store_explicit(object, desired, __ATOMIC_RELAXED)

#define ATOMIC_FLAG_INIT {0}
#define ATOMIC_VAR_INIT(value) (value)

typedef _Atomic(_Bool) atomic_bool;
typedef struct {
    atomic_bool _Value;
} atomic_flag;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(size_t) atomic_size_t;
typedef _Atomic(ptrdiff_t) atomic_ptrdiff_t;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(intmax_t) atomic_intmax_t;
typedef _Atomic(uintmax_t) atomic_uintmax_t;

#endif
