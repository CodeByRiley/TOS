/* userspace/include/math.h — minimal libm header.
 *
 * Only the entry points actually called by ported code (DOOM). All bodies
 * live in lib/math_stub.c. sqrt/fabs/floor/ceil work; the trig + pow are
 * stubs that return safe defaults. Replace with real libm when needed.
 */
#ifndef MATH_H
#define MATH_H

double sqrt(double x);
double pow(double x, double y);
double floor(double x);
double ceil(double x);
double fabs(double x);
double sin(double x);
double cos(double x);
double atan2(double y, double x);

#define M_PI 3.14159265358979323846

#endif
