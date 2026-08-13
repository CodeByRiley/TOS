#include <utilities/math.h>

#define TWO_PI 6.28318530717958647692
#define LN2    0.69314718055994530942

/*
 * Freestanding math helpers. These are small approximations, not a full IEEE
 * libm, but they never lower back into the public symbols below. That matters
 * in kernel code: a wrapper like floor() { return __builtin_floor(x); } can
 * compile into "call floor" and recurse until the boot stack is exhausted.
 */

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double floor(double x) {
    if (x != x) return x;
    if (x >= 9223372036854775807.0 || x <= -9223372036854775807.0)
        return x;

    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

double ceil(double x) {
    if (x != x) return x;
    if (x >= 9223372036854775807.0 || x <= -9223372036854775807.0)
        return x;

    long long i = (long long)x;
    if ((double)i < x) i++;
    return (double)i;
}

double sqrt(double x) {
    if (x <= 0.0) return 0.0;

    double g = x >= 1.0 ? x : 1.0;
    for (int i = 0; i < 32; i++)
        g = 0.5 * (g + x / g);
    return g;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;

    double qd = x / y;
    if (qd >= 9223372036854775807.0 || qd <= -9223372036854775807.0)
        return 0.0;

    long long q = (long long)qd;
    return x - (double)q * y;
}

static double cbrt_newton(double x) {
    if (x == 0.0) return 0.0;

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
        if (base == 0.0) return 0.0;
        base = 1.0 / base;
        e = (unsigned long long)(-expn);
    } else {
        e = (unsigned long long)expn;
    }

    while (e) {
        if (e & 1u) result *= base;
        base *= base;
        e >>= 1;
    }
    return result;
}

double log(double x) {
    if (x <= 0.0) return 0.0;

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
    if (x > 709.0) return 1.0e308;
    if (x < -745.0) return 0.0;

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
    if (expn == 0.0) return 1.0;
    if (base == 0.0) return expn > 0.0 ? 0.0 : 0.0;

    long long i = (long long)expn;
    if ((double)i == expn)
        return ipow(base, i);

    if (fabs(expn - (1.0 / 3.0)) < 0.000001)
        return cbrt_newton(base);

    if (base < 0.0) return 0.0;
    return exp(expn * log(base));
}

static double reduce_angle(double x) {
    x = fmod(x, TWO_PI);
    if (x > M_PI) x -= TWO_PI;
    if (x < -M_PI) x += TWO_PI;
    return x;
}

double sin(double x) {
    x = reduce_angle(x);
    double x2 = x * x;
    return x * (1.0
        - x2 / 6.0
        + (x2 * x2) / 120.0
        - (x2 * x2 * x2) / 5040.0
        + (x2 * x2 * x2 * x2) / 362880.0);
}

double cos(double x) {
    x = reduce_angle(x);
    double x2 = x * x;
    return 1.0
        - x2 / 2.0
        + (x2 * x2) / 24.0
        - (x2 * x2 * x2) / 720.0
        + (x2 * x2 * x2 * x2) / 40320.0;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return 0.0;
    return sin(x) / c;
}

static double atan_unit(double x) {
    double sign = x < 0.0 ? -1.0 : 1.0;
    x = fabs(x);

    if (x > 1.0)
        return sign * (M_PI_2 - atan_unit(1.0 / x));

    double x2 = x * x;
    double term = x;
    double sum = x;
    int add = 0;

    for (int n = 3; n <= 31; n += 2) {
        term *= x2;
        if (add)
            sum += term / (double)n;
        else
            sum -= term / (double)n;
        add = !add;
    }

    return sign * sum;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan_unit(y / x);
    if (x < 0.0 && y >= 0.0) return atan_unit(y / x) + M_PI;
    if (x < 0.0 && y < 0.0) return atan_unit(y / x) - M_PI;
    if (y > 0.0) return M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;
}

double asin(double x) {
    if (x > 1.0) x = 1.0;
    if (x < -1.0) x = -1.0;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    return M_PI_2 - asin(x);
}
