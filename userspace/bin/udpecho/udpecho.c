/* userspace/bin/udpecho/udpecho.c - echo UDP datagrams, via musl sockets.
 *
 * The point of this program is what it does NOT contain: no <lib/syscall.h>,
 * no TOS-specific call, nothing but <sys/socket.h> and <netinet/in.h>.
 * Every syscall it makes is issued by musl itself, on the Linux numbers -
 * socket is 41, bind 49, sendto 44, recvfrom 45. If those are not mirrored
 * in the kernel this program does not merely misbehave, it fails at the
 * first call with "unknown syscall", which is exactly how the gap showed up.
 *
 * That makes it the honest test for ported software. NetSurf's fetcher, and
 * anything else brought over unmodified, will reach the network through
 * these same entry points; a TOS-specific socket API would have proved
 * nothing about them.
 *
 * recvfrom does not block yet, so the loop polls. Once the socket layer
 * grows a wait queue this should drop the sleep and simply block.
 */
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ECHO_PORT 7777
#define ECHO_ROUNDS 4
#define POLL_SLEEP_US 10000

int main(int argc, char **argv) {
    int rounds = ECHO_ROUNDS;
    if (argc == 2) {
        rounds = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') {
                printf("udpecho: bad round count '%s'\n", argv[1]);
                return 1;
            }
            rounds = rounds * 10 + (*p - '0');
        }
        if (rounds <= 0) {
            printf("udpecho: rounds must be at least 1\n");
            return 1;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("udpecho: socket failed\n");
        return 1;
    }
    printf("udpecho: socket fd=%d\n", fd);

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(ECHO_PORT);
    local.sin_addr.s_addr = 0; /* INADDR_ANY */

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("udpecho: bind failed\n");
        close(fd);
        return 1;
    }
    printf("udpecho: listening on %d\n", ECHO_PORT);

    for (int i = 0; i < rounds; i++) {
        char buf[512];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        long n = -1;
        for (;;) {
            n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fromlen);
            if (n > 0)
                break;
            usleep(POLL_SLEEP_US);
        }

        printf("udpecho: got %ld bytes from port %d\n", n, ntohs(from.sin_port));

        /* Straight back to whoever sent it -- from is the proof that
         * recvfrom filled in a usable source address, not just a length. */
        long sent = sendto(fd, buf, (size_t)n, 0,
                           (struct sockaddr *)&from, fromlen);
        if (sent != n)
            printf("udpecho: sendto returned %ld for %ld bytes\n", sent, n);
        else
            printf("udpecho: echoed %ld bytes\n", sent);
    }

    close(fd);
    printf("udpecho: done\n");
    return 0;
}
