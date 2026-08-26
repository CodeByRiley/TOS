#include <string.h>
#include <unistd.h>

int main() {
    // Print to the serial port / tty
    const char *msg = "About to intentionally dereference a null pointer...\n";
    write(1, msg, strlen(msg));

    // Create a null pointer
    volatile int *bad_ptr = (volatile int *)0x0;

    // Try to write to it! This will instantly trigger a Page Fault.
    *bad_ptr = 0xDEADBEEF;

    // We should never reach this line
    return 0;
}
