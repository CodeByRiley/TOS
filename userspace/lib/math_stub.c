/* userspace/lib/math_stub.c — minimal libm subset for DOOM.
 *
 * Implements the handful of functions actually called by DOOM's renderer +
 * sound code. Uses small approximations rather than full IEEE-754 libm
 * implementations.
 */
#include "../include/math.h"
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#ifndef M_PI_4
#define M_PI_4 0.78539816339744830962
#endif

#define TWO_PI 6.28318530717958647692
#define LN2 0.69314718055994530942
#define HUGE_VAL 1.0e308

double sqrt(double x) {
  if (x <= 0.0)
    return 0.0;
  double r = x;
  for (int i = 0; i < 20; i++)
    r = 0.5 * (r + x / r);
  return r;
}

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
  if (x != x) return x; /* NaN check */
  if (x >= (double)LLONG_MAX || x <= (double)LLONG_MIN)
    return x;
  long long n = (long long)x;
  return (x < 0 && (double)n != x) ? (double)(n - 1) : (double)n;
}

double ceil(double x) {
  if (x != x) return x; /* NaN check */
  if (x >= (double)LLONG_MAX || x <= (double)LLONG_MIN)
    return x;
  long long n = (long long)x;
  return (x > 0 && (double)n != x) ? (double)(n + 1) : (double)n;
}

double fmod(double x, double y) {
  if (y == 0.0)
    return 0.0;

  double qd = x / y;
  if (qd >= (double)LLONG_MAX || qd <= (double)LLONG_MIN)
    return x;

  long long q = (long long)qd;
  return x - (double)q * y;
}

static double cbrt_newton(double x) {
  if (x == 0.0)
    return 0.0;

  double sign = x < 0.0 ? -1.0 : 1.0;
  double ax = fabs(x);
  double g = ax >= 1.0 ? ax : 1.0;

  for (int i = 0; i < 32; i++)
    g = (2.0 * g + ax / (g * g)) / 3.0;
  return sign * g;
}

static double ipow(double base, long long expn) {
  unsigned long long e;
  double result = 1.0;

  if (expn < 0) {
    if (base == 0.0)
      return HUGE_VAL;
    base = 1.0 / base;
    e = -(unsigned long long)expn;
  } else {
    e = (unsigned long long)expn;
  }

  while (e) {
    if (e & 1u)
      result *= base;
    base *= base;
    e >>= 1;
  }
  return result;
}

double log(double x) {
  if (x <= 0.0)
    return 0.0;

  int k = 0;
  while (x > 1.5) {
    x *= 0.5;
    k++;
  }
  while (x < 0.75) {
    x *= 2.0;
    k--;
  }

  double z = (x - 1.0) / (x + 1.0);
  double z2 = z * z;
  double term = z;
  double sum = 0.0;

  for (int n = 1; n <= 39; n += 2) {
    sum += term / (double)n;
    term *= z2;
  }

  return 2.0 * sum + (double)k * LN2;
}

double exp(double x) {
  if (x > 709.0)
    return HUGE_VAL;
  if (x < -745.0)
    return 0.0;

  int k = 0;
  while (x > LN2) {
    x -= LN2;
    k++;
  }
  while (x < -LN2) {
    x += LN2;
    k--;
  }

  double term = 1.0;
  double sum = 1.0;
  for (int i = 1; i <= 24; i++) {
    term *= x / (double)i;
    sum += term;
  }

  while (k > 0) {
    sum *= 2.0;
    k--;
  }
  while (k < 0) {
    sum *= 0.5;
    k++;
  }
  return sum;
}

double pow(double base, double expn) {
  if (expn == 0.0)
    return 1.0;
  if (base == 0.0)
    return (expn > 0.0) ? 0.0 : HUGE_VAL;

  long long i = (long long)expn;
  if ((double)i == expn)
    return ipow(base, i);

  if (fabs(expn - (1.0 / 3.0)) < 0.000001)
    return cbrt_newton(base);

  if (base < 0.0)
    return 0.0;
  return exp(expn * log(base));
}

static double reduce_angle(double x) {
  x = fmod(x, TWO_PI);
  if (x > M_PI)
    x -= TWO_PI;
  if (x < -M_PI)
    x += TWO_PI;
  return x;
}

double sin(double x) {
  x = reduce_angle(x);
  double x2 = x * x;
  return x * (1.0 - x2 / 6.0 + (x2 * x2) / 120.0 - (x2 * x2 * x2) / 5040.0 +
              (x2 * x2 * x2 * x2) / 362880.0 -
              (x2 * x2 * x2 * x2 * x2) / 39916800.0);
}

double cos(double x) {
  x = reduce_angle(x);
  double x2 = x * x;
  return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0 +
         (x2 * x2 * x2 * x2) / 40320.0 -
         (x2 * x2 * x2 * x2 * x2) / 3628800.0;
}

double tan(double x) {
  double c = cos(x);
  if (c == 0.0)
    return 0.0;
  return sin(x) / c;
}

static double atan_unit(double x) {
  if (x < 0.0)
    return -atan_unit(-x);
  if (x > 1.0)
    return M_PI_2 - atan_unit(1.0 / x);
  if (x > 0.5)
    return M_PI_4 + atan_unit((x - 1.0) / (x + 1.0));

  double x2 = x * x;
  double term = x;
  double sum = x;

  for (int n = 3; n <= 31; n += 2) {
    term *= x2;
    sum += (n % 4 == 1) ? (term / (double)n) : -(term / (double)n);
  }
  return sum;
}

double atan2(double y, double x) {
  if (x > 0.0)
    return atan_unit(y / x);
  if (x < 0.0 && y >= 0.0)
    return atan_unit(y / x) + M_PI;
  if (x < 0.0 && y < 0.0)
    return atan_unit(y / x) - M_PI;
  if (y > 0.0)
    return M_PI_2;
  if (y < 0.0)
    return -M_PI_2;
  return 0.0;
}

double asin(double x) {
  if (x > 1.0)
    x = 1.0;
  if (x < -1.0)
    x = -1.0;
  return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) { return M_PI_2 - asin(x); }
