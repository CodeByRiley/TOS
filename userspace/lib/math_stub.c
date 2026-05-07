#include "../include/math.h"

// crude approximations sufficient for DOOM
double sqrt(double x) {
    if (x <= 0) return 0;
    double r = x;
    for (int i = 0; i < 20; i++) r = 0.5 * (r + x/r);
    return r;
}

double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) { long n = (long)x; return (x < 0 && (double)n != x) ? (double)(n-1) : (double)n; }
double ceil(double x)  { long n = (long)x; return (x > 0 && (double)n != x) ? (double)(n+1) : (double)n; }

// stubs (DOOM rarely uses these in core gameplay)
double pow(double a, double b)   { (void)a; (void)b; return 1.0; }
double sin(double x)             { (void)x; return 0.0; }
double cos(double x)             { (void)x; return 1.0; }
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
