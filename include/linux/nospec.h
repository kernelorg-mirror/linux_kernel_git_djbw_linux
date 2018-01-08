// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.

#ifndef __NOSPEC_H__
#define __NOSPEC_H__

#include <linux/jump_label.h>
#include <asm/barrier.h>

/*
 * If idx is negative or if idx > size then bit 63 is set in the mask,
 * and the value of ~(-1L) is zero. When the mask is zero, bounds check
 * failed, __array_ptr will return NULL.
 */
#ifndef array_ptr_mask
#define array_ptr_mask(idx, sz)						\
({									\
	unsigned long mask;						\
	unsigned long _i = (idx);					\
	unsigned long _s = (sz);					\
									\
	mask = ~(long)(_i | (_s - 1 - _i)) >> (BITS_PER_LONG - 1);	\
	mask;								\
})
#endif

/**
 * __array_ptr - Generate a pointer to an array element, ensuring
 * the pointer is bounded under speculation to NULL.
 *
 * @base: the base of the array
 * @idx: the index of the element, must be less than LONG_MAX
 * @sz: the number of elements in the array, must be less than LONG_MAX
 *
 * If @idx falls in the interval [0, @sz), returns the pointer to
 * @arr[@idx], otherwise returns NULL.
 */
#define __array_ptr(base, idx, sz)					\
({									\
	union { typeof(*(base)) *_ptr; unsigned long _bit; } __u;	\
	typeof(*(base)) *_arr = (base);					\
	unsigned long _i = (idx);					\
	unsigned long _mask = array_ptr_mask(_i, (sz));			\
									\
	__u._ptr = _arr + (_i & _mask);					\
	__u._bit &= _mask;						\
	__u._ptr;							\
})

#if defined(ARCH_HAS_IFENCE) && !defined(ifence_array_ptr)
#error Arch claims ARCH_HAS_IFENCE, but does not implement ifence_array_ptr
#endif

#ifdef CONFIG_SPECTRE1_DYNAMIC
#ifndef HAVE_JUMP_LABEL
#error Compiler lacks asm-goto, can generate unsafe code
#endif

#ifdef CONFIG_SPECTRE1_IFENCE
DECLARE_STATIC_KEY_TRUE(nospec_key);
#else
DECLARE_STATIC_KEY_FALSE(nospec_key);
#endif

/*
 * The expectation is that no compiler or cpu will mishandle __array_ptr
 * leading to problematic speculative execution. Bypass the ifence
 * based implementation by default.
 */
#define array_ptr(base, idx, sz)				\
({								\
	typeof(*(base)) *__ret;					\
								\
	if (static_branch_unlikely(&nospec_key))		\
		__ret = ifence_array_ptr(base, idx, sz);	\
	else							\
		__ret = __array_ptr(base, idx, sz);		\
	__ret;							\
})
#else /* CONFIG_SPECTRE1_DYNAMIC */
/*
 * If jump labels are disabled we hard code either ifence_array_ptr or
 * array_ptr based on the config choice
 */
#ifdef CONFIG_SPECTRE1_IFENCE
#define array_ptr ifence_array_ptr
#else
/* fallback to __array_ptr by default */
#define array_ptr __array_ptr
#endif
#endif /* CONFIG_SPECTRE1_DYNAMIC */
#endif /* __NOSPEC_H__ */
