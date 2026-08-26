#include <stddef.h>

#include "utils/messages.h"

nserror netsurf_messages_add_from_file(const char *path);

nserror messages_add_from_file(const char *path)
{
    if (path == NULL) {
        path = "/res/netsurf/Messages";
    }
    return netsurf_messages_add_from_file(path);
}
