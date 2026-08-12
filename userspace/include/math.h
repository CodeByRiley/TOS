#ifndef MATH_H
#define MATH_H

#define LN2             0.69314718055994530942
#define M_PI            3.14159265358979323846
#define M_PI_DIV_2      1.57079632679489661923
#define M_PI_2          6.28318530717958647692
#define M_RAD_TO_DEG(x) ((x) * 180.0 / M_PI)
#define M_DEG_TO_RAD(x) ((x) * M_PI / 180.0)

double floor(double x);
double ceil(double x);
double fabs(double x);
double sqrt(double x);
double pow(double base, double exp);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double acos(double x);
double asin(double x);
double exp(double x);
double log(double x);


#endif
