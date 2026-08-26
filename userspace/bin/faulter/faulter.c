#include <string.h>
#include <unistd.h>

int main() {
    const char *msg = "About to intentionally dereference a null pointer...\n";
    write(1, msg, strlen(msg));

    volatile int *bad_ptr = (volatile int *)0x0;

    *bad_ptr = 0xDEADBEEF;

    return 0;
}
