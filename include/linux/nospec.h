// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.

#ifndef __NOSPEC_H__
#define __NOSPEC_H__

/*
 * When idx is out of bounds (idx >= sz), the sign bit will be set.
 * Extend the sign bit to all bits and invert, giving a result of zero
 * for an out of bounds idx, or ~0UL if within bounds [0, sz).
 */
#ifndef array_idx_mask
static inline unsigned long array_idx_mask(unsigned long idx, unsigned long sz)
{
	/*
	 * Warn developers about inappropriate array_idx usage.
	 *
	 * Even if the cpu speculates past the WARN_ONCE branch, the
	 * sign bit of idx is taken into account when generating the
	 * mask.
	 *
	 * This warning is compiled out when the compiler can infer that
	 * idx and sz are less than LONG_MAX.
	 */
	if (WARN_ONCE(idx > LONG_MAX || sz > LONG_MAX,
			"array_idx limited to range of [0, LONG_MAX]\n"))
		return 0;

	/*
	 * Always calculate and emit the mask even if the compiler
	 * thinks the mask is not needed. The compiler does not take
	 * into account the value of idx under speculation.
	 */
	OPTIMIZER_HIDE_VAR(idx);
	return ~(long)(idx | (sz - 1UL - idx)) >> (BITS_PER_LONG - 1);
}
#endif

/*
 * array_idx - sanitize an array index after a bounds check
 *
 * For a code sequence like:
 *
 *     if (idx < sz) {
 *         idx = array_idx(idx, sz);
 *         val = array[idx];
 *     }
 *
 * ...if the cpu speculates past the bounds check then array_idx() will
 * clamp the index within the range of [0, sz).
 */
#define array_idx(idx, sz)						\
({									\
	typeof(idx) _i = (idx);						\
	typeof(sz) _s = (sz);						\
	unsigned long _mask = array_idx_mask(_i, _s);			\
									\
	BUILD_BUG_ON(sizeof(_i) > sizeof(long));			\
	BUILD_BUG_ON(sizeof(_s) > sizeof(long));			\
									\
	_i &= _mask;							\
	_i;								\
})
#endif /* __NOSPEC_H__ */
