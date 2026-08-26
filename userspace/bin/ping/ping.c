/* userspace/bin/ping/ping.c - ICMP echo from userspace.
 *
 * This exists because ICMP echo reply was the one layer of the new stack
 * that shipped with no test behind it, and it was untestable for a dull
 * reason: QEMU's SLIRP backend does not forward inbound ICMP to the guest,
 * so `ping 10.0.2.30` from the host can never reach TOS however correct
 * the code is. The direction that does work is outbound - SLIRP answers
 * pings addressed to the gateway - and nothing in TOS could originate one.
 *
 * So the useful tool and the missing test are the same thing. Pinging
 * 10.0.2.2 exercises the entire stack end to end in one command: ARP
 * resolution, the IPv4 header and its checksum, transmit through the
 * driver's descriptor ring, receive, and the reply matching in icmp.c.
 *
 * No name resolution: there is no DNS yet, so the argument is a dotted
 * quad and saying so plainly beats a confusing failure later.
 */
#include <lib/syscall.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PING_DEFAULT_COUNT 4
#define PING_TIMEOUT_MS 1000
/* usleep rather than sleep_ticks: the PIT runs at 500 Hz today and
 * hardcoding that here would break silently the day it changes. */
#define PING_INTERVAL_MS 1000

/* Strict dotted quad. strtol would accept "1.2.3.4xyz" and octal-looking
 * components, both of which silently ping the wrong host. */
static int parse_ipv4(const char *text, unsigned char out[4]) {
    unsigned int part = 0;
    int digits = 0;
    int index = 0;

    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            part = part * 10u + (unsigned int)(*p - '0');
            if (++digits > 3 || part > 255u)
                return -1;
            continue;
        }

        if (*p == '.' || *p == '\0') {
            if (digits == 0 || index > 3)
                return -1;
            out[index++] = (unsigned char)part;
            part = 0;
            digits = 0;
            if (*p == '\0')
                break;
            continue;
        }

        return -1;
    }

    return index == 4 ? 0 : -1;
}

static void usage(void) {
    printf("usage: ping <dotted-quad> [count]\n");
    printf("  no DNS yet, so a name will not work.\n");
    printf("  10.0.2.2 is the gateway under QEMU user networking.\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage();
        return 1;
    }

    unsigned char dst[4];
    if (parse_ipv4(argv[1], dst) != 0) {
        printf("ping: %s is not a dotted-quad address\n", argv[1]);
        usage();
        return 1;
    }

    int count = PING_DEFAULT_COUNT;
    if (argc == 3) {
        count = 0;
        for (const char *p = argv[2]; *p; p++) {
            if (*p < '0' || *p > '9') {
                printf("ping: bad count '%s'\n", argv[2]);
                return 1;
            }
            count = count * 10 + (*p - '0');
            if (count > 10000) {
                printf("ping: count too large\n");
                return 1;
            }
        }
        if (count == 0) {
            printf("ping: count must be at least 1\n");
            return 1;
        }
    }

    /* The identifier separates our replies from any other pinger on the
     * segment. There is no getpid() worth using here, so the tick counter
     * is the cheapest thing that differs between runs. */
    unsigned short ident = (unsigned short)(get_ticks() & 0xFFFF);

    printf("PING %d.%d.%d.%d: %d data bytes\n",
           dst[0], dst[1], dst[2], dst[3], 32);

    int sent = 0;
    int received = 0;
    unsigned int rtt_total = 0;
    unsigned int rtt_min = 0xFFFFFFFFu;
    unsigned int rtt_max = 0;

    for (int i = 0; i < count; i++) {
        struct net_ping req;
        memset(&req, 0, sizeof(req));
        memcpy(req.dst, dst, 4);
        req.ident = ident;
        req.seq = (unsigned short)(i + 1);
        req.timeout_ms = PING_TIMEOUT_MS;

        long rc = net_ping(&req);
        sent++;

        if (rc == 0) {
            received++;
            rtt_total += req.rtt_ms;
            if (req.rtt_ms < rtt_min)
                rtt_min = req.rtt_ms;
            if (req.rtt_ms > rtt_max)
                rtt_max = req.rtt_ms;
            printf("32 bytes from %d.%d.%d.%d: icmp_seq=%d time=%ums\n",
                   dst[0], dst[1], dst[2], dst[3], req.seq, req.rtt_ms);
        } else if (rc == -2) {
            /* No route is permanent for this destination, so stop rather
             * than repeat an identical failure `count` times. */
            printf("ping: no route to %d.%d.%d.%d (is the link up?)\n",
                   dst[0], dst[1], dst[2], dst[3]);
            break;
        } else {
            printf("request timed out: icmp_seq=%d\n", req.seq);
        }

        if (i + 1 < count)
            usleep(PING_INTERVAL_MS * 1000);
    }

    printf("\n--- %d.%d.%d.%d ping statistics ---\n",
           dst[0], dst[1], dst[2], dst[3]);
    int lost = sent - received;
    printf("%d transmitted, %d received, %d%% packet loss\n",
           sent, received, sent ? (lost * 100) / sent : 0);
    if (received)
        printf("rtt min/avg/max = %u/%u/%ums\n",
               rtt_min, rtt_total / (unsigned int)received, rtt_max);

    return received ? 0 : 1;
}
