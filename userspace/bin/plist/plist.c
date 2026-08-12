/* userspace/bin/plist/plist.c — list the kernel task table.
 *
 * usage: plist [pattern]
 *
 * With a pattern, only rows whose name contains it are shown. Data comes
 * from SYS_PROC_LIST, which snapshots the scheduler's task array.
 */
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "../../lib/syscall.h"

/* sys_proc_list caps its reply at 64 rows and MAX_TASKS is 16 today, so a
 * fixed buffer covers every possible answer without an allocation that
 * could fail partway through a diagnostic tool. */
#define MAX_PROCS 64

static const char *state_str(int s) {
    switch (s) {
    case PROC_STATE_RUNNING:  return "RUNNING";
    case PROC_STATE_BLOCKED:  return "BLOCKED";
    case PROC_STATE_ZOMBIE:   return "ZOMBIE";
    case PROC_STATE_READY:    return "READY";
    case PROC_STATE_DEAD:     return "DEAD";
    case PROC_STATE_SLEEPING: return "SLEEPING";
    case PROC_STATE_LOADING:  return "LOADING";
    default:                  return "UNKNOWN";
    }
}

/* Kernel threads spawned without task_set_name carry an empty name — the
 * tty and framebuffer-flush threads both land there. A blank column reads
 * as a bug, so say what they are instead. */
static const char *display_name(const char *name) {
    return name[0] ? name : "(kthread)";
}

int main(int argc, char **argv) {
    const char *pattern = argc > 1 ? argv[1] : 0;
    static struct proc_info procs[MAX_PROCS];

    long n = proc_list(procs, MAX_PROCS);
    if (n < 0) {
        printf("plist: could not read the process table\n");
        return 1;
    }

    printf("%-6s %-6s %-9s %-12s %s\n", "PID", "PPID", "STATE", "TICKS", "NAME");

    long shown = 0;
    for (long i = 0; i < n; i++) {
        /* task_set_name always terminates, but this buffer crossed a
         * syscall boundary — bound it before any str* call trusts it. */
        procs[i].name[PROC_NAME_MAX - 1] = '\0';

        if (pattern && !strstr(procs[i].name, pattern))
            continue;

        printf("%-6d %-6d %-9s %-12llu %s\n",
               procs[i].pid,
               procs[i].parent_pid,
               state_str(procs[i].state),
               (unsigned long long)procs[i].ticks_run,
               display_name(procs[i].name));
        shown++;
    }

    if (pattern && shown == 0) {
        printf("plist: no process matches \"%s\"\n", pattern);
        return 1;
    }
    return 0;
}
