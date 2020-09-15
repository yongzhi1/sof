/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2020 Google LLC. All rights reserved.
 *
 * Author: Pin-chih Lin <johnylin@google.com>
 */
#ifndef __SOF_AUDIO_DRC_DRC_MATH_H__
#define __SOF_AUDIO_DRC_DRC_MATH_H__

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define DRC_PI_FLOAT 3.141592653589793f
#define DRC_PI_OVER_TWO_FLOAT 1.57079632679489661923f
#define DRC_TWO_OVER_PI_FLOAT 0.63661977236758134f
#define DRC_NEG_TWO_DB 0.7943282347242815f /* -2dB = 10^(-2/20) */

#ifndef max
#define max(a, b)                                                              \
	({                                                                     \
		__typeof__(a) _a = (a);                                        \
		__typeof__(b) _b = (b);                                        \
		_a > _b ? _a : _b;                                             \
	})
#endif

#ifndef min
#define min(a, b)                                                              \
	({                                                                     \
		__typeof__(a) _a = (a);                                        \
		__typeof__(b) _b = (b);                                        \
		_a < _b ? _a : _b;                                             \
	})
#endif

static inline float decibels_to_linear(float decibels)
{
	/* 10^(x/20) = e^(x * log(10^(1/20))) */
	return expf(0.1151292546497022f * decibels);
}

static inline float linear_to_decibels(float linear)
{
	/* For negative or zero, just return a very small dB value. */
	if (linear <= 0)
		return -1000;

	/* 20 * log10(x) = 20 / log(10) * log(x) */
	return 8.6858896380650366f * logf(linear);
}

static inline float warp_sinf(float x)
{
	return sinf(DRC_PI_OVER_TWO_FLOAT * x);
}

static inline float warp_asinf(float x)
{
	return asinf(x) * DRC_TWO_OVER_PI_FLOAT;
}

static inline float knee_expf(float input)
{
	return expf(input);
}

static inline int isbadf(float x)
{
	return x != 0 && !isnormal(x);
}

#endif //  __SOF_AUDIO_DRC_DRC_MATH_H__
