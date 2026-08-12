/* Heavy in-guest regression test for VM, FAT, IPC, Winman and task churn. */
#include "../../include/fcntl.h"
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "../../lib/syscall.h"
#include "../../lib/wm.h"
#include <stdint.h>

#define PAGE_SIZE 4096
#define MEMORY_ROUNDS 96
#define MEMORY_SLOTS 12
#define FILE_ROUNDS 64
#define SHMEM_ROUNDS 128
#define WINDOW_ROUNDS 64
#define SYNC_ELF_ROUNDS 32
#define SYNC_PE_ROUNDS 8
#define ASYNC_ROUNDS 64
#define ASYNC_BATCH 4

#define IPC_STRESS_READY (IPC_USER_FIRST + 0x20)
#define IPC_STRESS_PROBE (IPC_USER_FIRST + 0x21)
#define IPC_STRESS_ACK   (IPC_USER_FIRST + 0x22)
#define IPC_STRESS_STOP  (IPC_USER_FIRST + 0x23)

static int failures;
static uint8_t file_buffer[8192];
static uint8_t file_readback[8192];

static void check(int condition, const char *message) {
    if (condition)
        return;
    printf("stress: FAIL %s\n", message);
    failures++;
}

static uint64_t memory_pattern(int round, int slot, uint64_t index) {
    return 0xD6E8FEB86659FD93ULL * (index + 1) ^
           ((uint64_t)(uint32_t)round << 32) ^ (uint32_t)slot;
}

static int memory_stress(void) {
    void *maps[MEMORY_SLOTS];
    size_t lengths[MEMORY_SLOTS];
    struct mem_stats before = {0};
    struct mem_stats after = {0};
    check(mem_stats(&before) == 0, "read memory stats before VM churn");

    for (int round = 0; round < MEMORY_ROUNDS; round++) {
        for (int slot = 0; slot < MEMORY_SLOTS; slot++) {
            size_t pages = (size_t)slot + 1;
            size_t length = pages * PAGE_SIZE;
            uint64_t *mapping = mmap(0, length, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS);
            if (mapping == MAP_FAILED) {
                check(0, "mmap during VM churn");
                return -1;
            }
            maps[slot] = mapping;
            lengths[slot] = length;

            for (size_t page = 0; page < pages; page++)
                check(mapping[page * PAGE_SIZE / sizeof(uint64_t)] == 0,
                      "fresh anonymous pages are zeroed");

            size_t words = length / sizeof(uint64_t);
            for (size_t i = 0; i < words; i++)
                mapping[i] = memory_pattern(round, slot, i);
            check(mprotect(mapping, length, PROT_READ) == 0,
                  "drop write permission during VM churn");
            for (size_t i = 0; i < words; i += 31)
                check(mapping[i] == memory_pattern(round, slot, i),
                      "read-only mapping retains its data");
            check(mprotect(mapping, length, PROT_READ | PROT_WRITE) == 0,
                  "restore write permission during VM churn");
        }

        for (int slot = MEMORY_SLOTS - 1; slot >= 0; slot--)
            check(munmap(maps[slot], lengths[slot]) == 0,
                  "munmap during VM churn");
    }

    check(mem_stats(&after) == 0, "read memory stats after VM churn");
    if (before.frame_size && after.used_frames > before.used_frames)
        check(after.used_frames - before.used_frames <= 8,
              "VM churn returns leaf frames to the PMM");

    printf("stress: memory PASS rounds=%d\n", MEMORY_ROUNDS);
    return 0;
}

static void file_path(char path[24], int index) {
    const char base[] = "/STRESS/FILE0.BIN";
    memcpy(path, base, sizeof(base));
    path[12] = (char)('0' + index);
}

static int file_stress(void) {
    for (int i = 0; i < 8; i++) {
        char path[24];
        file_path(path, i);
        unlink(path);
    }
    rmdir_path("/STRESS");
    check(mkdir_path("/STRESS") == 0, "create stress directory");

    for (int round = 0; round < FILE_ROUNDS; round++) {
        char path[24];
        file_path(path, round & 7);
        for (size_t i = 0; i < sizeof(file_buffer); i++)
            file_buffer[i] = (uint8_t)(round * 17 + (int)i * 29);

        int fd = (int)open(path, O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0) {
            check(0, "open stress file");
            return -1;
        }
        check(write(fd, file_buffer, sizeof(file_buffer)) ==
              (long)sizeof(file_buffer), "write multi-cluster stress file");
        check(lseek(fd, 0, SEEK_SET) == 0, "rewind stress file");
        memset(file_readback, 0, sizeof(file_readback));
        check(read(fd, file_readback, sizeof(file_readback)) ==
              (long)sizeof(file_readback), "read multi-cluster stress file");
        check(memcmp(file_buffer, file_readback, sizeof(file_buffer)) == 0,
              "stress file readback matches");
        check(close(fd) == 0, "close stress file");
        check(unlink(path) == 0, "unlink stress file");
    }

    check(rmdir_path("/STRESS") == 0, "remove stress directory");
    printf("stress: filesystem PASS rounds=%d\n", FILE_ROUNDS);
    return 0;
}

static int pid_present(int pid) {
    struct proc_info rows[32];
    long count = proc_list(rows, 32);
    if (count < 0)
        return 0;
    for (long i = 0; i < count; i++)
        if (rows[i].pid == pid)
            return 1;
    return 0;
}

static int wait_pid_gone(int pid, int ticks) {
    for (int i = 0; i < ticks; i++) {
        if (!pid_present(pid))
            return 1;
        sleep_ticks(1);
    }
    return 0;
}

static int wait_message(uint32_t type, int from_pid, struct ipc_msg *out,
                        int ticks) {
    for (int i = 0; i < ticks; i++) {
        struct ipc_msg message;
        while (ipc_recv(&message) > 0) {
            if (message.type == type && (int)message.from_pid == from_pid) {
                if (out)
                    *out = message;
                return 1;
            }
        }
        sleep_ticks(1);
    }
    return 0;
}

static uint64_t shared_pattern(uint32_t seed, uint64_t index) {
    return 0x9E3779B97F4A7C15ULL * (index + 1) ^
           ((uint64_t)seed << 32) ^ seed;
}

static int shmem_stress(void) {
    char parent_pid[16];
    snprintf(parent_pid, sizeof(parent_pid), "%d", (int)get_pid());
    char *peer_argv[] = {"stress_peer", parent_pid, 0};
    int peer = (int)spawn("/usr/bin/stress_peer.elf", peer_argv);
    check(peer > 0, "spawn shared-memory peer");
    if (peer <= 0)
        return -1;
    check(wait_message(IPC_STRESS_READY, peer, 0, 2000),
          "shared-memory peer becomes ready");

    for (int round = 0; round < SHMEM_ROUNDS; round++) {
        int pages = 1 + (round & 7);
        size_t length = (size_t)pages * PAGE_SIZE;
        uint64_t *owner = mmap(0, length, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS);
        if (owner == MAP_FAILED) {
            check(0, "allocate shared-memory owner pages");
            break;
        }
        uint32_t seed = (uint32_t)(round * 131 + 7);
        uint64_t words = length / sizeof(uint64_t);
        for (uint64_t i = 0; i < words; i++)
            owner[i] = shared_pattern(seed, i);

        uint64_t peer_va = 0;
        check(shmem_share(peer, (uint64_t)(uintptr_t)owner, pages, &peer_va) == 0,
              "share owner pages into peer");

        struct ipc_msg probe = {0};
        probe.type = IPC_STRESS_PROBE;
        probe.a = pages;
        probe.b = (int32_t)seed;
        probe.c = round;
        probe.va = peer_va;
        check(ipc_send(peer, &probe) == 0, "send shared-memory probe");

        struct ipc_msg ack;
        int got_ack = wait_message(IPC_STRESS_ACK, peer, &ack, 2000);
        check(got_ack, "receive shared-memory acknowledgement");
        if (got_ack)
            check(ack.a == round && ack.b == 1,
                  "peer verified the shared-memory pattern");
        if (got_ack && ack.b == 1) {
            for (uint64_t i = 0; i < words; i += 17)
                check(owner[i] == ~shared_pattern(seed, i),
                      "peer writes are visible to the owner");
        }

        check(shmem_unshare(peer, peer_va, pages) == 0,
              "revoke peer shared-memory mapping");
        check(munmap(owner, length) == 0, "release shared-memory owner pages");
    }

    struct ipc_msg stop = {0};
    stop.type = IPC_STRESS_STOP;
    check(ipc_send(peer, &stop) == 0, "stop shared-memory peer");
    check(wait_pid_gone(peer, 2000), "shared-memory peer is reaped");
    printf("stress: shmem PASS rounds=%d\n", SHMEM_ROUNDS);
    return 0;
}

static int window_stress(void) {
    check(wm_pid() > 0, "Winman is registered");
    for (int round = 0; round < WINDOW_ROUNDS; round++) {
        struct wm_window window;
        int width = 240 + (round & 3) * 16;
        int height = 160 + (round & 1) * 16;
        if (wm_window_create(width, height, "stress", &window) != 0) {
            check(0, "create stress window");
            return -1;
        }
        uint32_t *pixels = (uint32_t *)(uintptr_t)window.surface_va;
        size_t count = (size_t)width * (size_t)height;
        uint32_t color = 0x00010101u * (uint32_t)(round + 1);
        for (size_t i = 0; i < count; i++)
            pixels[i] = color ^ (uint32_t)i;
        check(wm_window_invalidate(window.handle) == 0,
              "invalidate stress window");
        check(wm_window_set_title(window.handle, "stress-active") == 0,
              "rename stress window");
        check(wm_window_destroy(window.handle) == 0,
              "destroy stress window");
    }
    printf("stress: windows PASS rounds=%d\n", WINDOW_ROUNDS);
    return 0;
}

static int process_stress(void) {
    char *hello_argv[] = {"hello", 0};
    for (int i = 0; i < SYNC_ELF_ROUNDS; i++)
        check(exec("/usr/bin/hello.elf", hello_argv) == 42,
              "synchronous ELF child exits with expected code");
    printf("stress: sync ELF PASS rounds=%d\n", SYNC_ELF_ROUNDS);

    for (int i = 0; i < SYNC_PE_ROUNDS; i++)
        check(exec("/usr/bin/hello.exe", hello_argv) == 42,
              "synchronous PE child exits with expected code");
    printf("stress: sync PE PASS rounds=%d\n", SYNC_PE_ROUNDS);

    for (int launched = 0; launched < ASYNC_ROUNDS; launched += ASYNC_BATCH) {
        int pids[ASYNC_BATCH];
        for (int i = 0; i < ASYNC_BATCH; i++) {
            pids[i] = (int)spawn("/usr/bin/hello.elf", hello_argv);
            check(pids[i] > 0, "reserve asynchronous child");
        }
        for (int i = 0; i < ASYNC_BATCH; i++) {
            if (pids[i] > 0)
                check(wait_pid_gone(pids[i], 3000),
                      "asynchronous child is activated and reaped");
        }
    }
    printf("stress: async PASS rounds=%d\n", ASYNC_ROUNDS);
    return 0;
}

int main(int argc, char **argv) {
    printf("stress: begin\n");
    if (argc > 1 && strcmp(argv[1], "windows") == 0) {
        window_stress();
        if (failures) {
            printf("stress: FAILURES=%d\n", failures);
            return 1;
        }
        printf("stress: PASS\n");
        return 0;
    }

    memory_stress();
    file_stress();
    shmem_stress();
    window_stress();
    process_stress();

    if (failures) {
        printf("stress: FAILURES=%d\n", failures);
        return 1;
    }
    printf("stress: PASS\n");
    return 0;
}
