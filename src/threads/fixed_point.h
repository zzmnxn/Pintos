#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define F (1 << 14)

/* Convert integer n to fixed-point. */
#define INT_TO_FP(n) ((n) * F)

/* Convert fixed-point x to integer (rounding toward zero). */
#define FP_TO_INT_ZERO(x) ((x) / F)

/* Convert fixed-point x to integer (rounding to nearest). */
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

/* Add/Subtract two fixed-point numbers. */
#define ADD_FP(x, y) ((x) + (y))
#define SUB_FP(x, y) ((x) - (y))

/* Add/Subtract a fixed-point number and an integer. */
#define ADD_INT(x, n) ((x) + (n) * F)
#define SUB_INT(x, n) ((x) - (n) * F)

/* Multiply two fixed-point numbers. */
#define MUL_FP(x, y) (((int64_t) (x)) * (y) / F)

/* Multiply a fixed-point number by an integer. */
#define MUL_INT(x, n) ((x) * (n))

/* Divide two fixed-point numbers. */
#define DIV_FP(x, y) (((int64_t) (x)) * F / (y))

/* Divide a fixed-point number by an integer. */
#define DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed_point.h */
