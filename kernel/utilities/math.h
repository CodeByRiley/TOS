#ifndef MATH_H
#define MATH_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

/* Freestanding math declarations */
double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double fmod(double x, double y);
double log(double x);
double exp(double x);
double pow(double base, double expn);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);

#endif /* MATH_H */
