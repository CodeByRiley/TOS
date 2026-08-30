#ifndef __APP_INFO__H__
#define __APP_INFO__H__

#include <stdint.h>

enum app_type {
    APP_TYPE_INTERNAL = 0,  /* winman, holyd, fdchild, stress_peer... */
    APP_TYPE_CLI      = 1,  /* cat, ls, btop */
    APP_TYPE_GUI      = 2,  /* user-launchable windowed apps */
    APP_TYPE_TEST     = 3,  /* mtest, vmtest, pe_test, stress... */
    APP_TYPE_DAEMON   = 4,
};

struct app_info {
    uint32_t type;
    char     name[32];      /* display name for the menu */
};

#define APP_INFO(type_, name_) \
    __attribute__((used, section(".appinfo"), aligned(4))) \
    static const struct app_info _app_info_ = { (type_), (name_) }

#endif
