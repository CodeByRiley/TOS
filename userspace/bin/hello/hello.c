#include "../../lib/syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *msg = "hello from ring 3\n";
    long len = 0;
    while (msg[len]) len++;
    write(1, msg, len);
    return 42;
}
