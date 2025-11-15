#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* Fixed-point format: 17.14 
   f = 2^14 = 16384 */
#define F (1 << 14)

/* Convert integer n to fixed-point. */
#define INT_TO_FP(n) ((n) * F)

/* Convert fixed-point x to integer (round toward zero). */
#define FP_TO_INT_ZERO(x) ((x) / F)

/* Convert fixed-point x to integer (round to nearest). */
#define FP_TO_INT_NEAREST(x) (((x) >= 0) ? (((x) + F / 2) / F) : (((x) - F / 2) / F))

/* Add two fixed-point numbers. */
#define ADD_FP(x, y) ((x) + (y))

/* Subtract two fixed-point numbers. */
#define SUB_FP(x, y) ((x) - (y))

/* Add integer n to fixed-point x. */
#define ADD_INT(x, n) ((x) + (n) * F)

/* Subtract integer n from fixed-point x. */
#define SUB_INT(x, n) ((x) - (n) * F)

/* Multiply two fixed-point numbers (using 64-bit to prevent overflow). */
#define MUL_FP(x, y) ((int) (((int64_t) (x)) * (y) / F))

/* Divide two fixed-point numbers (using 64-bit to prevent overflow). */
#define DIV_FP(x, y) ((int) (((int64_t) (x)) * F / (y)))

/* Multiply fixed-point x by integer n. */
#define MUL_INT(x, n) ((x) * (n))

/* Divide fixed-point x by integer n. */
#define DIV_INT(x, n) ((x) / (n))

#endif 

