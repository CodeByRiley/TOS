/* Executable lookup, launch, inspection, and bounded lifecycle waits. */
#ifndef TOS_PROCESS_H
#define TOS_PROCESS_H

#include <lib/syscall.h>
#include <stddef.h>
#include <stdint.h>

#define PROCESS_PATH_MAX 260
#define PROCESS_DEFAULT_PATH "/bin:/usr/bin:/usr/local/bin:/system/bin"
#define PROCESS_WAIT_FOREVER UINT32_MAX

enum process_wait_result {
    PROCESS_ERROR   = -1,
    PROCESS_TIMEOUT = 0,
    PROCESS_EXITED  = 1,
};

/* Resolve `program` directly when it contains a slash, otherwise search a
 * colon-separated path. NULL/empty search_path uses PROCESS_DEFAULT_PATH.
 * An extensionless name probes the exact spelling, then .elf and .exe. */
int process_resolve(const char *program, const char *search_path,
                    char *out, size_t capacity);

long process_spawn(const char *program, char *const argv[],
                   const char *search_path);
long process_exec(const char *program, char *const argv[],
                  const char *search_path);

long process_snapshot(struct proc_info *out, size_t capacity);
int process_get(int pid, struct proc_info *out);
int process_is_alive(int pid);

/* Wait until a process disappears or reaches ZOMBIE/DEAD. TOS does not yet
 * expose child exit status through proc_list, so this reports lifecycle only.
 * timeout_ticks == 0 performs one poll. */
int process_wait(int pid, uint32_t timeout_ticks, struct proc_info *last_seen);

#endif
