#include "../../lib/syscall.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"

extern int printf(const char *, ...);

static void elfreboot(int time) {
	if (time < 0) time = 0;

    if (time == 0) printf("rebooting now...\n");
    else           printf("rebooting in %d seconds...\n", time);

    sys_reboot(time);
}

int main(int argc, char **argv) {
    int time = 0;

    /* Accepted forms:
     *   reboot
     *   reboot N
     *   reboot -t N
     */
    if (argc == 1) {
        time = 0;
    } else if (argc == 2) {
        time = atoi(argv[1]);
    } else if (argc == 3 && strcmp(argv[1], "-t") == 0) {
        time = atoi(argv[2]);
    } else {
        printf("usage: reboot [-t SECONDS]\n");
        return 1;
    }

    elfreboot(time);
    return 0;     /* never reached */
}
