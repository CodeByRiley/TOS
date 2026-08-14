#include <unistd.h>

int main(void) {
    static const char msg[] = "hello from musl on TOS\n";
    ssize_t n = write(1, msg, sizeof(msg) - 1);
    return n == (ssize_t)(sizeof(msg) - 1) ? 0 : 1;
}
