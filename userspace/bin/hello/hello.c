/* userspace/bin/hello/hello.c — smoke-test "hello world".
 *
 * Writes a fixed string to stdout via raw write() (no printf, no libc
 * pull-in beyond syscall.h). Returns 42 so the shell shows a recognisable
 * non-zero exit code, confirming the user→kernel→user round trip.
 */
#include "../../lib/syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *msg = "hello from ring 3\n";
    long len = 0;
    while (msg[len]) len++;
    write(1, msg, len);
    return 42;
}
