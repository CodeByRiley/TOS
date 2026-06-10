/* userspace/include/strings.h — POSIX <strings.h> alias.
 *
 * strcasecmp/strncasecmp etc. already live in <string.h> in this libc, so
 * this header is a one-liner indirection for source compatibility with
 * code that includes <strings.h> explicitly.
 */
#ifndef STRINGS_H
#define STRINGS_H

#include <string.h>

#endif
