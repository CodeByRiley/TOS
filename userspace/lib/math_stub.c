/* userspace/lib/math_stub.c — minimal libm subset for DOOM.
 *
 * Only the handful of functions actually called by DOOM's renderer +
 * sound code. sqrt() uses Newton's method; trig is stubbed because the
 * core gameplay path doesn't hit it. Replace with a real libm before
 * shipping anything else that needs accurate math.
 */
#include "../include/math.h"

/* 20 iterations of Newton's method on x. Returns 0 for x <= 0. */
double sqrt(double x) {
    if (x <= 0) return 0;
    double r = x;
    for (int i = 0; i < 20; i++) r = 0.5 * (r + x/r);
    return r;
}

/* Absolute value. */
double fabs(double x) { return x < 0 ? -x : x; }

/* Largest integer <= x, returned as double. */
double floor(double x) { long n = (long)x; return (x < 0 && (double)n != x) ? (double)(n-1) : (double)n; }

/* Smallest integer >= x, returned as double. */
double ceil(double x)  { long n = (long)x; return (x > 0 && (double)n != x) ? (double)(n+1) : (double)n; }

/* Stubs — not reached by DOOM's hot path. Return safe defaults. */
double pow(double a, double b)   { (void)a; (void)b; return 1.0; }
double sin(double x)             { (void)x; return 0.0; }
double cos(double x)             { (void)x; return 1.0; }
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
