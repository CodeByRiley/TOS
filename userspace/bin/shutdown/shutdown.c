/* userspace/bin/shutdown/shutdown.c , `shutdown` command.
 *
 * Thin wrapper over SYS_SHUTDOWN. Accepts a delay and a free-form reason
 * string the kernel logs before powering off. Doesn't return on success.
 */
#include <lib/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trigger the shutdown syscall after printing a status line. */
static void elfshutdown(int time, const char *reason) {
    if (time < 0) time = 0;

    if (time == 0) printf("shutting down now (%s)...\n", reason);
    else           printf("shutting down in %d seconds (%s)...\n", time, reason);

    sys_shutdown(time, reason);
}

/* Accepted forms:
 *   shutdown
 *   shutdown N
 *   shutdown -t N
 *   shutdown -t N -r REASON
 *   shutdown -r REASON
 */
int main(int argc, char **argv) {
    int          time   = 0;
    const char  *reason = "user";

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            time = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            reason = argv[i + 1];
            i += 2;
        } else if (i == 1) {
            time = atoi(argv[i]);
            i++;
        } else {
            printf("usage: shutdown [-t SECONDS] [-r REASON]\n");
            return 1;
        }
    }

    elfshutdown(time, reason);
    return 0;     /* never reached */
}
