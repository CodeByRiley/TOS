#ifndef APP_INFO_H
#define APP_INFO_H

#include <include/sys/types.h>
#include <stdint.h>

#define APP_INFO_MAGIC   UINT32_C(0x54414931) /* "TAI1" */
#define APP_INFO_SECTION ".appinfo"

enum app_type {
    APP_TYPE_INTERNAL = 0,
    APP_TYPE_CLI      = 1,
    APP_TYPE_GUI      = 2,
    APP_TYPE_TEST     = 3,
    APP_TYPE_DAEMON   = 4,
};

struct app_info {
    uint32_t magic;
    uint32_t type;
    char     name[32];
};

extern const struct app_info __appinfo_start[] WEAK;
extern const struct app_info __appinfo_end[]   WEAK;

#define APP_INFO_JOIN_(a, b) a##b
#define APP_INFO_JOIN(a, b)  APP_INFO_JOIN_(a, b)

#define APP_INFO(type_, name_)                                             \
    __attribute__((used, section(APP_INFO_SECTION), aligned(4)))           \
    static const struct app_info                                           \
    APP_INFO_JOIN(_app_info_, __COUNTER__) = {                              \
        .magic = APP_INFO_MAGIC,                                            \
        .type  = (type_),                                                   \
        .name  = (name_),                                                   \
    }

_Static_assert(sizeof(struct app_info) == 40,
               "unexpected struct app_info size");
#endif
