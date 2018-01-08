// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.

#ifndef __NOSPEC_H__
#define __NOSPEC_H__

#include <linux/jump_label.h>
#include <asm/barrier.h>

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

#ifdef CONFIG_SPECTRE1_IFENCE
DECLARE_STATIC_KEY_TRUE(nospec_key);
#else
DECLARE_STATIC_KEY_FALSE(nospec_key);
#endif

#ifdef ifence_array_ptr
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
#else
#define array_ptr __array_ptr
#endif

#endif /* __NOSPEC_H__ */
