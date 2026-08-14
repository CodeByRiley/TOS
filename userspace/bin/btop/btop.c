/* userspace/bin/btop/btop.c — terminal task & memory monitor.
 *
 * Polls SYS_PROC_LIST + SYS_MEM_STATS every ~500 ms and redraws a header,
 * memory bar, and process table. CPU% per task is the ticks_run delta
 * divided by the sum of all task deltas across the sample window, so the
 * column always sums to 100% (including the idle task).
 *
 * Foreground console app. Pushes the shell's screen onto the alt-screen
 * stack on entry and restores it on exit. Quits on ESC.
 */
// #region INCLUDES
#include <lib/syscall.h>
#include <lib/console.h>
#include <lib/keymap.h>
#include <include/key_codes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
// #endregion INCLUDES

// #region EXTERNS
extern int    vsnprintf(char *, size_t, const char *, va_list);
extern size_t strlen(const char *);
extern void  *memset(void *, int, size_t);
// #endregion EXTERNS

// #region CONSTANTS
#define MAX_PROCS    16
#define BAR_WIDTH    40
#define REFRESH_TICK 50          /* 100 Hz PIT * 50 = 500 ms */
// #endregion CONSTANTS

// #region PRINTF WRAPPER
/* Local printf that goes through console_write (no stdout pollution if
 * something later in the stack redirects fd 1). */
static int bp_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf, r);
    return r;
}

// #endregion PRINTF WRAPPER

// #region HELPERS
/* Map PROC_STATE_* → fixed-width 4-char label for the table column. */
static const char *state_name(int s) {
    switch (s) {
    case PROC_STATE_RUNNING:  return "RUN ";
    case PROC_STATE_BLOCKED:  return "BLK ";
    case PROC_STATE_ZOMBIE:   return "ZOMB";
    case PROC_STATE_READY:    return "RDY ";
    case PROC_STATE_DEAD:     return "DEAD";
    case PROC_STATE_SLEEPING: return "SLP ";
    case PROC_STATE_LOADING:  return "LOAD";
    default:                  return "??? ";
    }
}

/* Non-blocking ESC check. Drains the keyboard ring without sleeping so
 * the redraw loop stays responsive. */
static int esc_pressed(void) {
    int pressed;
    uint16_t key;
    while (kbd_poll(&pressed, &key)) {
        if (pressed && key == KEY_ESC) return 1;
    }
    return 0;
}

// #endregion HELPERS

// #region RENDERERS
/* Print the "MEM [####....] used / total KiB (pct%)" line. */
static void draw_mem_bar(const struct mem_stats *m) {
    uint64_t total = m->total_frames;
    uint64_t used  = m->used_frames;
    if (total == 0) total = 1;
    if (used > total) used = total;

    int filled = (int)((used * (uint64_t)BAR_WIDTH) / total);
    char bar[BAR_WIDTH + 1];
    for (int i = 0; i < BAR_WIDTH; i++) bar[i] = i < filled ? '#' : '.';
    bar[BAR_WIDTH] = 0;

    uint64_t used_kb  = (used  * m->frame_size) / 1024;
    uint64_t total_kb = (total * m->frame_size) / 1024;
    uint64_t pct      = (used * 100) / total;

    bp_printf("MEM [%s] %lu / %lu KiB (%lu%%)\n",
              bar, (unsigned long)used_kb, (unsigned long)total_kb,
              (unsigned long)pct);
}

/* Render the per-process table header + rows. */
static void draw_procs(const struct proc_info *cur, int n,
                       const uint64_t *delta, uint64_t total_delta) {
    bp_printf(" PID PPID STATE  CPU%%  NAME\n");
    bp_printf("----+----+------+-----+----------------\n");
    for (int i = 0; i < n; i++) {
        unsigned pct = 0;
        if (total_delta > 0) {
            pct = (unsigned)((delta[i] * 100) / total_delta);
        }
        bp_printf("%4d %4d %-4s   %3u%%  %s\n",
                  cur[i].pid, cur[i].parent_pid,
                  state_name(cur[i].state),
                  pct, cur[i].name[0] ? cur[i].name : "(?)");
    }
}

// #endregion RENDERERS

// #region MAIN
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct proc_info prev[MAX_PROCS];
    struct proc_info cur[MAX_PROCS];
    uint64_t         delta[MAX_PROCS];
    int              prev_n = 0;

    memset(prev, 0, sizeof(prev));

    /* Take the screen — saves the shell's grid + cursor and clears the
     * live screen. Restored before every `return` so the prompt comes
     * back exactly where it left off. */
    console_save();

    for (;;) {
        if (esc_pressed()) {
            console_restore();
            return 0;
        }

        struct mem_stats mem;
        if (mem_stats(&mem) != 0) {
            console_restore();
            bp_printf("btop: mem_stats failed\n");
            return 1;
        }

        long n = proc_list(cur, MAX_PROCS);
        if (n < 0) {
            console_restore();
            bp_printf("btop: proc_list failed\n");
            return 1;
        }

        /* Compute per-task tick deltas vs. the previous snapshot. Match
         * by PID, not by index — a task reaped between snapshots shifts
         * everything below it up. New tasks report 0% this tick. */
        uint64_t total_delta = 0;
        for (int i = 0; i < n; i++) {
            uint64_t prev_ticks = 0;
            for (int j = 0; j < prev_n; j++) {
                if (prev[j].pid == cur[i].pid) {
                    prev_ticks = prev[j].ticks_run;
                    break;
                }
            }
            delta[i] = cur[i].ticks_run >= prev_ticks
                       ? cur[i].ticks_run - prev_ticks
                       : 0;
            total_delta += delta[i];
        }

        console_clear();
        bp_printf("btop  uptime=%lu ms   procs=%d   [ESC to quit]\n",
                  (unsigned long)get_ticks(), (int)n);
        bp_printf("\n");
        draw_mem_bar(&mem);
        bp_printf("\n");
        draw_procs(cur, (int)n, delta, total_delta);

        /* Carry the snapshot forward as next tick's baseline. */
        for (int i = 0; i < n; i++) prev[i] = cur[i];
        prev_n = (int)n;

        sleep_ticks(REFRESH_TICK);
    }
}
// #endregion MAIN
