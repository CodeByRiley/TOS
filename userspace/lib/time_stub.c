#include "../include/time.h"
#include "syscall.h"

time_t time(time_t *t) {
    time_t v = get_ticks() / 1000;
    if (t) *t = v;
    return v;
}
