/* Shared-memory verifier used by stress.elf. */
#include <stdio.h>
#include <stdlib.h>
#include <lib/syscall.h>
#include <stdint.h>

#define IPC_STRESS_READY (IPC_USER_FIRST + 0x20)
#define IPC_STRESS_PROBE (IPC_USER_FIRST + 0x21)
#define IPC_STRESS_ACK   (IPC_USER_FIRST + 0x22)
#define IPC_STRESS_STOP  (IPC_USER_FIRST + 0x23)

static uint64_t expected_word(uint32_t seed, uint64_t index) {
    return 0x9E3779B97F4A7C15ULL * (index + 1) ^
           ((uint64_t)seed << 32) ^ seed;
}

static void send_reply(int parent_pid, uint32_t type, int round, int ok) {
    struct ipc_msg reply = {0};
    reply.type = type;
    reply.a = round;
    reply.b = ok;
    ipc_send(parent_pid, &reply);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("stress_peer: missing parent pid\n");
        return 1;
    }

    int parent_pid = atoi(argv[1]);
    if (parent_pid <= 0)
        return 1;

    send_reply(parent_pid, IPC_STRESS_READY, 0, 1);

    for (;;) {
        struct ipc_msg message;
        if (ipc_recv(&message) <= 0) {
            sleep_ticks(1);
            continue;
        }
        if ((int)message.from_pid != parent_pid)
            continue;
        if (message.type == IPC_STRESS_STOP)
            break;
        if (message.type != IPC_STRESS_PROBE || message.a <= 0 || !message.va)
            continue;

        uint64_t words = (uint64_t)(uint32_t)message.a * 4096 / sizeof(uint64_t);
        uint64_t *shared = (uint64_t *)(uintptr_t)message.va;
        int ok = 1;
        for (uint64_t i = 0; i < words; i++) {
            uint64_t expected = expected_word((uint32_t)message.b, i);
            if (shared[i] != expected) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            for (uint64_t i = 0; i < words; i++)
                shared[i] = ~expected_word((uint32_t)message.b, i);
        }
        send_reply(parent_pid, IPC_STRESS_ACK, message.c, ok);
    }

    printf("stress_peer: clean exit\n");
    return 0;
}
