/* userspace/bin/sh/sh.c — tsh: toy interactive shell.
 *
 * Reads a line, tokenises it, dispatches shell-owned built-ins through a
 * table, and falls through to a filesystem ELF lookup so `btop` runs
 * btop.elf without a `run` prefix.
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
#include "utilities/types.h"
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

        if (ctrl_held && k == KEY_MINUS) {
            console_zoom_out();
            continue;
        }

        if (ctrl_held && k == KEY_EQUAL) {
            console_zoom_in();
            continue;
        }

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

struct builtin {
    const char *name;
    const char *usage;
    int (*fn)(int argc, char **argv);
};

static int builtin_help(int argc, char **argv);
static int builtin_mkdir(int argc, char **argv);
static int builtin_echo(int argc, char **argv);
static int builtin_write(int argc, char **argv);
static int builtin_rm(int argc, char **argv);
static int builtin_clear(int argc, char **argv);
static int builtin_run(int argc, char **argv);
static int builtin_test(int argc, char **argv);
static int builtin_fdnstest(int argc, char **argv);
static int builtin_exit(int argc, char **argv);
static int builtin_cd(int argc, char **argv);
static int builtin_pwd(int argc, char **argv);

static const struct builtin builtins[] = {
    { "help",     "help",                       builtin_help },
    { "clear",    "clear",                      builtin_clear },
    { "exit",     "exit",                       builtin_exit },
    { "run",      "run PATH[.elf] [ARG...] [&]", builtin_run },
    { "mkdir",    "mkdir DIR",                  builtin_mkdir },
    { "rm",       "rm FILE",                    builtin_rm },
    { "echo",     "echo TEXT...",               builtin_echo },
    { "write",    "write FILE TEXT...",         builtin_write },
    { "test",     "test",                       builtin_test },
    { "fdnstest", "fdnstest",                   builtin_fdnstest },
    { "cd",  "cd DIR", builtin_cd },
    { "pwd", "pwd",    builtin_pwd },
    { 0, 0, 0 },
};

static int shell_should_exit = 0;

static const struct builtin *find_builtin(const char *name) {
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(name, builtins[i].name) == 0)
            return &builtins[i];
    }
    return 0;
}

static int builtin_help(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("builtins:\n");
    for (int i = 0; builtins[i].name; i++)
        printf("  %s\n", builtins[i].usage);
    printf("external programs: type an ELF basename or path\n");
    return 0;
}

static int builtin_mkdir(int argc, char **argv) {
    if (argc < 2) { printf("usage: mkdir DIR\n"); return 1; }
    if (mkdir_path(argv[1]) != 0)
        printf("mkdir: %s: failed\n", argv[1]);
    return 0;
}

static int builtin_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) console_putc(' ');
        console_puts(argv[i]);
    }
    console_putc('\n');
    return 0;
}

static int builtin_write(int argc, char **argv) {
    if (argc < 3) { printf("usage: write FILE TEXT...\n"); return 1; }
    void *fp = fopen(argv[1], "w");
    if (!fp) { printf("write: %s: open failed\n", argv[1]); return 1; }
    for (int i = 2; i < argc; i++) {
        if (i > 2) fwrite(" ", 1, 1, fp);
        fwrite(argv[i], 1, strlen(argv[i]), fp);
    }
    fwrite("\n", 1, 1, fp);
    fclose(fp);
    return 0;
}

static int builtin_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm FILE\n"); return 1; }
    if (unlink(argv[1]) != 0) {
        printf("rm: %s: not found\n", argv[1]);
        return 1;
    }
    return 0;
}

static int builtin_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_clear();
    return 0;
}

static int builtin_exit(int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell_should_exit = 1;
    return 0;
}

static int builtin_cd(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/";
    if (chdir(path) != 0) {
        printf("cd: %s: failed\n", path);
        return 1;
    }
    return 0;
}

static int builtin_pwd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char buf[256];
    if (!getcwd(buf, sizeof(buf))) {
        printf("pwd: failed\n");
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}

// #endregion BUILT-INS

// #region EXEC

static const char *exec_search_paths[] = {
    "",
    "/",
    "BIN",
    "GAMES",
    0,
};

static int append_char(char *out, int *n, int max, char c) {
    if (*n + 1 >= max)
        return -1;
    out[(*n)++] = c;
    out[*n] = 0;
    return 0;
}

static int append_str(char *out, int *n, int max, const char *s) {
    while (*s) {
        if (append_char(out, n, max, *s++) != 0)
            return -1;
    }
    return 0;
}

static int has_path_separator(const char *s) {
    while (*s) {
        if (*s == '/' || *s == '\\')
            return 1;
        s++;
    }
    return 0;
}

static int final_component_has_dot(const char *s) {
    int has_dot = 0;
    while (*s) {
        if (*s == '/' || *s == '\\')
            has_dot = 0;
        else if (*s == '.')
            has_dot = 1;
        s++;
    }
    return has_dot;
}

static int build_exec_candidate(const char *prefix, const char *raw,
                                char *out, int max) {
    int n = 0;
    out[0] = 0;

    if (prefix && prefix[0] && strcmp(prefix, ".") != 0) {
        if (append_str(out, &n, max, prefix) != 0)
            return -1;

        if (out[n - 1] != '/') {
            if (append_char(out, &n, max, '/') != 0)
                return -1;
        }
    }

    if (append_str(out, &n, max, raw) != 0)
        return -1;

    if (!final_component_has_dot(raw)) {
        if (append_str(out, &n, max, ".elf") != 0)
            return -1;
    }

    return 0;
}

static int probe_exec_candidate(const char *path) {
    long fd = open(path, 0);
    if (fd < 0)
        return 0;
    close((int)fd);
    return 1;
}

/* Resolve a user command to an ELF path. The filesystem is the command
 * registry: names without a slash are searched in a tiny PATH list, while
 * explicit paths are used as-is after optional .elf suffixing. */
static int resolve_exec_path(const char *raw, char *out, int max) {
    if (!raw || !raw[0])
        return -1;

    if (has_path_separator(raw)) {
        if (build_exec_candidate("", raw, out, max) != 0)
            return -1;
        return probe_exec_candidate(out) ? 0 : -1;
    }

    for (int i = 0; exec_search_paths[i]; i++) {
        if (build_exec_candidate(exec_search_paths[i], raw, out, max) != 0)
            continue;
        if (probe_exec_candidate(out))
            return 0;
    }
    return -1;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

/* Resolve argv[0] as an ELF path and run it.
 *
 *   - Searches a small PATH for bare names.
 *   - Preserves explicit directory components.
 *   - Appends ".elf" to the final component if it has no extension.
 *   - Probes existence via open() so typos surface as a clean error
 *     rather than the kernel's "[x.elf exited -1]" message.
 *
 * When `bg != 0` the child is launched via spawn() (fire-and-forget) so
 * windowed apps don't pin the prompt. The trailing `&` argv token sets
 * bg upstream. */
static int exec_argv(int argc, char **argv, int bg) {
    if (argc < 1 || !argv[0] || !argv[0][0]) return -1;
    static char fixed[256];
    if (resolve_exec_path(argv[0], fixed, sizeof(fixed)) != 0)
        return -1;

    /* Lowercase base (sans extension) for child's argv[0]. */
    const char *base = path_basename(fixed);
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
    //console_clear();
    printf("[%s exited %d]\n", fixed, (int)code);
    return 0;
}

/* `run PATH[.elf] [ARGS...] [&]` — explicit form. Strips the leading
 * "run" token and forwards the rest to exec_argv. */
static int builtin_run(int argc, char **argv) {
    if (argc < 2) { printf("usage: run PATH[.elf] [ARG...] [&]\n"); return 1; }
    int bg = 0;
    if (argc >= 2 && argv[argc - 1] && strcmp(argv[argc - 1], "&") == 0) {
        bg = 1;
        argc--;
    }
    return exec_argv(argc - 1, argv + 1, bg);
}

static int builtin_test(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char a_buf[5] = {0};
    char b_buf[5] = {0};

    int a = (int)open("readme.txt", 0);
    int b = (int)open("readme.txt", 0);

    if (a < 0 || b < 0) {
        printf("fdtest: open failed a=%d b=%d\n", a, b);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        return 1;
    }

    long ar = read(a, a_buf, 4);
    long br = read(b, b_buf, 4);

    printf("fdtest: a='%s' b='%s'\n", a_buf, b_buf);

    if (ar == 4 && br == 4 && strcmp(a_buf, b_buf) == 0) {

        sh_printf("fdtest: PASS independent open offsets\n");
    }
    else {
        sh_printf("fdtest: FAIL ar=%d br=%d\n", (int)ar, (int)br);
    }

    close(a);
    close(b);
    return (ar == 4 && br == 4 && strcmp(a_buf, b_buf) == 0) ? 0 : 1;
}

static int builtin_fdnstest(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int parent_fd = (int)open("readme.txt", 0);
    if (parent_fd < 0) {
        printf("fdnstest: parent open failed\n");
        return 1;
    }

    printf("fdnstest: parent fd=%d\n", parent_fd);

    char *child_argv[] = { "fdchild", 0 };
    long code = exec("fdchild.elf", child_argv);

    printf("fdnstest: child exited %d\n", (int)code);
    close(parent_fd);
    return code == 0 ? 0 : 1;
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

        const struct builtin *builtin = find_builtin(cmd);
        if (builtin) {
            builtin->fn(ac, targs);
            if (shell_should_exit) {
                printf("exiting SHELF\n");
                return 0;
            }
        } else {
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
