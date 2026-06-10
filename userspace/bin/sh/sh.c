/* userspace/bin/sh/sh.c — tsh: toy interactive shell.
 *
 * Reads a line, tokenises it, dispatches a small set of built-ins
 * (ls/cat/echo/write/rm/clear/help/exit), and falls through to a
 * filesystem ELF lookup so `btop` runs BTOP.ELF without a `run` prefix.
 * Foreground execs block on exec(); a trailing `&` token launches via
 * spawn() so windowed apps don't pin the prompt.
 *
 * Input model:
 *   - Raw KEY_* events from kbd_poll, folded to ASCII via keymap.c.
 *   - Shift/Ctrl tracked locally.
 *   - TAB cycles filename completion (FAT16 root only).
 */
// #region INCLUDES
#include "../../lib/syscall.h"
#include "../../lib/keymap.h"
#include "../../lib/console.h"
#include "../../include/key_codes.h"
#include <stdarg.h>
// #endregion INCLUDES

// #region EXTERNS
extern int    vsnprintf(char *, size_t, const char *, va_list);
extern void  *fopen(const char *, const char *);
extern size_t fread(void *, size_t, size_t, void *);
extern size_t fwrite(const void *, size_t, size_t, void *);
extern int    fclose(void *);
extern void  *malloc(size_t);
extern void   free(void *);
extern size_t strlen(const char *);
extern int    strcmp(const char *, const char *);
extern char  *strchr(const char *, int);
extern void  *memset(void *, int, size_t);
// #endregion EXTERNS

// #region PRINTF WRAPPER
/* Local printf bound to the console (not stdout) so anything writing to
 * fd 1 elsewhere doesn't mix into the shell's UI. */
static int sh_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf, r);
    return r;
}
#define printf  sh_printf
#define con_out(s, n) console_write((s), (n))
// #endregion PRINTF WRAPPER

// #region GLOBALS
#define LINE_MAX 512

static int shift_held = 0;
static int ctrl_held  = 0;
// #endregion GLOBALS

// #region KEYBOARD INPUT
/* Block until a printable / control character is pressed. Tracks Shift +
 * Ctrl modifier state inline; returns ASCII for any printable key. */
static char read_char(void) {
    int      pressed;
    uint16_t k;
    while (1) {
        if (!kbd_poll(&pressed, &k)) { sleep_ticks(1); continue; }

        if (k == KEY_LEFTSHIFT || k == KEY_RIGHTSHIFT) {
            shift_held = pressed;
            continue;
        }
        if (k == KEY_LEFTCTRL || k == KEY_RIGHTCTRL) {
            ctrl_held = pressed;
            continue;
        }
        if (!pressed) continue;       /* only act on press */

        char c = keymap_to_ascii(k, shift_held);
        if (c) return c;
    }
}

// #endregion KEYBOARD INPUT

// #region TAB COMPLETION
#define COMP_MAX_MATCHES 32
#define COMP_NAME_MAX    16          /* FAT 8.3 + dot + nul slack */

/* Cache across consecutive TAB presses so cycling matches doesn't rescan
 * the filesystem. Invalidated by any non-TAB key (read_line clears it). */
static char comp_matches[COMP_MAX_MATCHES][COMP_NAME_MAX];
static int  comp_count = 0;
static int  comp_index = 0;
static int  comp_word_len = 0;       /* length of word that was replaced */

/* Index of first character of the last whitespace-delimited word in
 * buf[0..n]. */
static int word_start(const char *buf, int n) {
    int i = n;
    while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') i--;
    return i;
}

/* Case-insensitive prefix match. plen passed separately so prefix can be
 * a substring of buf without a NUL boundary. */
static int starts_with_ci(const char *name, const char *prefix, int plen) {
    for (int i = 0; i < plen; i++) {
        char a = name[i];
        char b = prefix[i];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Populate comp_matches with every root-FS name that starts (case-
 * insensitively) with the `plen` chars at `prefix`. */
static void comp_scan(const char *prefix, int plen) {
    comp_count = 0;
    comp_index = 0;

    char     dbuf[512];
    unsigned idx = 0;
    while (comp_count < COMP_MAX_MATCHES) {
        long n = readdir(&idx, dbuf, sizeof(dbuf));
        if (n <= 0) break;
        long off = 0;
        while (off < n && comp_count < COMP_MAX_MATCHES) {
            const char *name = dbuf + off;
            size_t nl = strlen(name);
            if (nl > 0 && nl < COMP_NAME_MAX &&
                starts_with_ci(name, prefix, plen)) {
                for (size_t k = 0; k <= nl; k++) {
                    comp_matches[comp_count][k] = name[k];
                }
                comp_count++;
            }
            off += (long)nl + 1;
        }
    }
}

/* Erase the previously-completed word from buf and the screen, then write
 * comp_matches[comp_index] in its place. */
static void comp_apply(char *buf, int *n_ptr, int max) {
    int n     = *n_ptr;
    int wstart = n - comp_word_len;
    if (wstart < 0) wstart = 0;

    for (int i = 0; i < comp_word_len; i++) console_puts("\b \b");

    const char *m  = comp_matches[comp_index];
    int         ml = (int)strlen(m);
    if (wstart + ml >= max) ml = max - wstart - 1;
    if (ml < 0) ml = 0;

    for (int i = 0; i < ml; i++) {
        buf[wstart + i] = m[i];
        console_putc(m[i]);
    }
    *n_ptr        = wstart + ml;
    comp_word_len = ml;
}

/* TAB handler. First press snapshots the word and scans the FS; further
 * consecutive presses (no other input in between) cycle the match list. */
static void handle_tab(char *buf, int *n_ptr, int max, int continuing) {
    int n = *n_ptr;
    if (!continuing) {
        int ws  = word_start(buf, n);
        int wl  = n - ws;
        char prefix[COMP_NAME_MAX];
        if (wl >= COMP_NAME_MAX) wl = COMP_NAME_MAX - 1;
        for (int i = 0; i < wl; i++) prefix[i] = buf[ws + i];
        prefix[wl] = 0;

        comp_word_len = wl;
        comp_scan(prefix, wl);
        if (comp_count == 0) return;     /* no matches; leave buf as-is */
        comp_index = 0;
        comp_apply(buf, n_ptr, max);
    } else {
        if (comp_count == 0) return;
        comp_index = (comp_index + 1) % comp_count;
        comp_apply(buf, n_ptr, max);
    }
}

// #endregion TAB COMPLETION

// #region LINE INPUT
/* Read a line into buf. Handles backspace + TAB locally; returns length
 * (excluding the trailing NUL). Echoes characters as it goes. */
static int read_line(char *buf, int max) {
    int n            = 0;
    int last_was_tab = 0;
    while (1) {
        char c = read_char();
        if (c != '\t') last_was_tab = 0;
        if (c == '\n') {
            console_putc('\n');
            buf[n] = 0;
            return n;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                console_puts("\b \b");
            }
            continue;
        }
        if (c == '\t') {
            handle_tab(buf, &n, max, last_was_tab);
            last_was_tab = 1;
            continue;
        }
        if (n + 1 < max) {
            buf[n++] = c;
            console_putc(c);
        }
    }
}

// #endregion LINE INPUT

// #region TOKENIZER
/* Split `line` into argv-style tokens in place. Pointers in argv alias
 * into line, which is modified (NUL inserted at each separator). */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    return argc;
}

// #endregion TOKENIZER

// #region BUILT-INS

static void builtin_help(void) {
    printf("commands:\n");
    printf("  help          this message\n");
    printf("  ls            list files\n");
    printf("  cat FILE      print file contents\n");
    printf("  echo TEXT...  print arguments\n");
    printf("  write FILE T  write text T to FILE (overwrite)\n");
    printf("  rm FILE       delete file\n");
    printf("  run PATH.ELF  spawn child ELF, return when it exits\n");
    printf("  clear         clear screen\n");
    printf("  reboot   -t TIME    reboot the system\n");
    printf("  shutdown -t TIME -r REASON shutdown the system\n");
    printf("  exit          quit shell\n");
}

static void builtin_ls(void) {
    char     buf[512];
    unsigned idx = 0;
    while (1) {
        long n = readdir(&idx, buf, sizeof(buf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            const char *name = buf + off;
            printf("  %s\n", name);
            off += strlen(name) + 1;
        }
    }
}

static void builtin_cat(int argc, char **argv) {
    if (argc < 2) { printf("usage: cat FILE\n"); return; }
    void *fp = fopen(argv[1], "r");
    if (!fp) { printf("cat: %s: open failed\n", argv[1]); return; }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        console_write(buf, n);
    }
    fclose(fp);
}

static void builtin_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) console_putc(' ');
        console_puts(argv[i]);
    }
    console_putc('\n');
}

static void builtin_write(int argc, char **argv) {
    if (argc < 3) { printf("usage: write FILE TEXT...\n"); return; }
    void *fp = fopen(argv[1], "w");
    if (!fp) { printf("write: %s: open failed\n", argv[1]); return; }
    for (int i = 2; i < argc; i++) {
        if (i > 2) fwrite(" ", 1, 1, fp);
        fwrite(argv[i], 1, strlen(argv[i]), fp);
    }
    fwrite("\n", 1, 1, fp);
    fclose(fp);
}

static void builtin_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm FILE\n"); return; }
    if (unlink(argv[1]) != 0) {
        printf("rm: %s: not found\n", argv[1]);
    }
}

static void builtin_clear(void) {
    console_clear();
}

// #endregion BUILT-INS

// #region EXEC

/* exec a sibling ELF, passing args through. `prog_name` becomes child argv[0]
 * (display-friendly lowercase basename). */
static void run_elf(const char *path, const char *prog_name,
                    int argc, char **argv) {
    static char *child[16];
    int n = 0;
    child[n++] = (char *)prog_name;
    for (int i = 1; i < argc && n < 15; i++) {
        child[n++] = argv[i];
    }
    child[n] = 0;
    long code = exec(path, child);
    console_clear();
    printf("[%s exited %ld]\n", prog_name, code);
}

static void builtin_reboot(int argc, char **argv) {
    run_elf("REBOOT.ELF", "reboot", argc, argv);
}

static void builtin_shutdown(int argc, char **argv) {
    run_elf("SHUTDOWN.ELF", "shutdown", argc, argv);
}

/* Resolve argv[0] as a path to an ELF on the root FS and run it.
 *
 *   - Strips directory components (FAT16 here is flat root only).
 *   - Uppercases the basename and appends ".ELF" if there's no extension.
 *   - Probes existence via open() so typos surface as a clean error
 *     rather than the kernel's "[X.ELF exited -1]" message.
 *
 * When `bg != 0` the child is launched via spawn() (fire-and-forget) so
 * windowed apps don't pin the prompt. The trailing `&` argv token sets
 * bg upstream. */
static int exec_argv(int argc, char **argv, int bg) {
    if (argc < 1 || !argv[0] || !argv[0][0]) return -1;
    const char *raw = argv[0];

    const char *base = raw;
    for (const char *p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    if (!*base) return -1;

    /* Uppercase base into fixed[]; track if extension already present. */
    static char fixed[16];
    int  fl = 0;
    int  has_dot = 0;
    while (fl < (int)sizeof(fixed) - 5 && base[fl]) {
        char c = base[fl];
        if (c == '.') has_dot = 1;
        if (c >= 'a' && c <= 'z') c -= 32;
        fixed[fl++] = c;
    }
    if (!has_dot) {
        fixed[fl++] = '.';
        fixed[fl++] = 'E';
        fixed[fl++] = 'L';
        fixed[fl++] = 'F';
    }
    fixed[fl] = 0;

    long probe = open(fixed, 0);
    if (probe < 0) return -1;
    close((int)probe);

    /* Lowercase base (sans extension) for child's argv[0]. */
    static char prog_name[16];
    int  pn = 0;
    while (pn < (int)sizeof(prog_name) - 1 && base[pn] && base[pn] != '.') {
        char c = base[pn];
        if (c >= 'A' && c <= 'Z') c += 32;
        prog_name[pn] = c;
        pn++;
    }
    prog_name[pn] = 0;

    static char *child_argv[16];
    int n = 0;
    child_argv[n++] = prog_name;
    for (int j = 1; j < argc && n < 15; j++) {
        child_argv[n++] = argv[j];
    }
    child_argv[n] = 0;

    if (bg) {
        long pid = spawn(fixed, child_argv);
        if (pid < 0) {
            printf("%s: spawn failed\n", fixed);
            return -1;
        }
        printf("[%s pid=%d &]\n", fixed, (int)pid);
        return 0;
    }
    long code = exec(fixed, child_argv);
    console_clear();
    printf("[%s exited %d]\n", fixed, (int)code);
    return 0;
}

/* `run PATH[.ELF] [ARGS...] [&]` — explicit form. Strips the leading
 * "run" token and forwards the rest to exec_argv. */
static void builtin_run(int argc, char **argv) {
    if (argc < 2) { printf("usage: run PATH[.ELF] [ARG...] [&]\n"); return; }
    int bg = 0;
    if (argc >= 2 && argv[argc - 1] && strcmp(argv[argc - 1], "&") == 0) {
        bg = 1;
        argc--;
    }
    exec_argv(argc - 1, argv + 1, bg);
}

// #endregion EXEC

// #region MAIN
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    console_init();
    sh_printf("shelf v0.1 - type 'help'\n");

    char  line[LINE_MAX];
    char *targs[16];

    while (1) {
        console_puts("$ ");
        int len = read_line(line, sizeof(line));
        if (len == 0) continue;

        int  ac = tokenize(line, targs, 16);
        if (ac == 0) continue;
        char *cmd = targs[0];

        if (strcmp(cmd, "ls")    == 0)         { printf("running command ls\n");        builtin_ls(); }
        else if (strcmp(cmd, "help")  == 0)    { printf("running command help\n");      builtin_help(); }
        else if (strcmp(cmd, "clear") == 0)    { printf("running command clear\n");     builtin_clear(); }
        else if (strcmp(cmd, "rm")    == 0)    { printf("running command rm\n");        builtin_rm(ac, targs); }
        else if (strcmp(cmd, "run")   == 0)    { printf("running command run\n");       builtin_run(ac, targs); }
        else if (strcmp(cmd, "cat")   == 0)    { printf("running command cat\n");       builtin_cat(ac, targs); }
        else if (strcmp(cmd, "echo")  == 0)    { printf("running command echo\n");      builtin_echo(ac, targs); }
        else if (strcmp(cmd, "write") == 0)    { printf("running command write\n");     builtin_write(ac, targs); }
        else if (strcmp(cmd, "reboot") == 0)   { printf("running command reboot\n");    builtin_reboot(ac, targs); }
        else if (strcmp(cmd, "shutdown") == 0) { printf("running command shutdown\n");  builtin_shutdown(ac, targs); }
        else if (strcmp(cmd, "exit")  == 0)    { printf("exitting SHELF"); return 0; }
        else {
            /* No built-in matched — try filesystem lookup. A trailing
             * `&` token means launch backgrounded via spawn(). */
            int bg = 0;
            int eff_ac = ac;
            if (eff_ac >= 1 && targs[eff_ac - 1] &&
                strcmp(targs[eff_ac - 1], "&") == 0) {
                bg = 1;
                eff_ac--;
            }
            if (exec_argv(eff_ac, targs, bg) != 0) {
                printf("%s: command not found\n", cmd);
            }
        }
    }
}
// #endregion MAIN
