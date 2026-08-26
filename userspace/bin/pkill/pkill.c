/* userspace/bin/pkill/pkill.c — kill processes by pid or name.
 *
 * usage: pkill <pid>
 *        pkill <name>      kills every process whose name contains <name>
 *
 * An all-digits argument is treated as a pid, anything else as a name
 * pattern. Name matching is substring, so `pkill sh` also hits `shutdown`
 * — list first with `plist <name>` if that matters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lib/syscall.h>

/* Matches sys_proc_list's own cap. */
#define MAX_PROCS 64

/* SIGKILL. process_kill maps this to exit code 128 + signal. */
#define PKILL_SIGNAL 9

/* The kernel refuses to kill anything without a user PML4, so every kernel
 * thread is already protected in task_kill. This list only exists to fail
 * with a clear message instead of a bare -1. */
static int is_protected_name(const char *name) {
    return !strcmp(name, "init") || !strcmp(name, "idle");
}

static int is_all_digits(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: pkill <pid|name>\n");
        return 1;
    }

    const char *target = argv[1];
    int by_pid = is_all_digits(target);
    int want_pid = by_pid ? atoi(target) : 0;

    if (by_pid && want_pid < 1) {
        printf("pkill: %s is not a valid pid\n", target);
        return 1;
    }

    static struct proc_info procs[MAX_PROCS];
    long n = proc_list(procs, MAX_PROCS);
    if (n < 0) {
        printf("pkill: could not read the process table\n");
        return 1;
    }

    int matched = 0;
    int failed = 0;

    for (long i = 0; i < n; i++) {
        procs[i].name[PROC_NAME_MAX - 1] = '\0';

        if (by_pid) {
            if (procs[i].pid != want_pid) continue;
        } else {
            if (!strstr(procs[i].name, target)) continue;
        }
        matched++;

        if (is_protected_name(procs[i].name)) {
            printf("pkill: refusing to kill %s (pid %d)\n",
                   procs[i].name, procs[i].pid);
            failed++;
            continue;
        }

        /* On one CPU, the running entry is pkill itself. */
        if (procs[i].state == PROC_STATE_RUNNING) {
            printf("pkill: not killing myself (pid %d)\n", procs[i].pid);
            continue;
        }

        if (procs[i].state == PROC_STATE_ZOMBIE) {
            printf("pkill: %s (pid %d) has already exited\n",
                   procs[i].name, procs[i].pid);
            continue;
        }

        if (kill(procs[i].pid, PKILL_SIGNAL) < 0) {
            printf("pkill: could not kill %s (pid %d)\n",
                   procs[i].name, procs[i].pid);
            failed++;
        } else {
            printf("pkill: killed %s (pid %d)\n",
                   procs[i].name, procs[i].pid);
        }
    }

    if (matched == 0) {
        if (by_pid)
            printf("pkill: no process with pid %d\n", want_pid);
        else
            printf("pkill: no process matches \"%s\"\n", target);
        return 1;
    }

    return failed ? 1 : 0;
}
