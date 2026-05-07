/*
 * tsh — toy shell.
 * Built-ins only (no exec): ls, cat, echo, write, rm, clear, help, exit.
 */
#include "../../lib/syscall.h"
#include "../../lib/keymap.h"
#include "../../lib/console.h"
#include "../../include/key_codes.h"

#include <stdarg.h>

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

/* shell wrappers around reboot.elf / shutdown.elf — pass argv straight through */

/* Local shell printf -> console (NOT serial) */
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

#define LINE_MAX 512

static int shift_held = 0;
static int ctrl_held  = 0;

/* Block until a printable / control char is pressed; return ASCII. */
static char read_char(void) {
    int      pressed;
    uint16_t k;
    while (1) {
        if (!kbd_poll(&pressed, &k)) continue;

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

/* Read line into buf, return length (excluding trailing nul). */
static int read_line(char *buf, int max) {
    int n = 0;
    while (1) {
        char c = read_char();
        if (c == '\n') {
            console_putc('\n');
            buf[n] = 0;
            return n;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                /* erase last char on screen: \b space \b */
                console_puts("\b \b");
            }
            continue;
        }
        if (n + 1 < max) {
            buf[n++] = c;
            console_putc(c);
        }
    }
}

/* Tokenize line into argv-style array. argv[i] points into line (modified). */
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

/* built-ins */
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

/* invoke a separate ELF, passing through args; argv[0] becomes prog_name */
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

static void builtin_run(int argc, char **argv) {
    if (argc < 2) { printf("usage: run PATH[.ELF] [ARG...]\n"); return; }

    const char *raw = argv[1];

    /* strip directory components — FAT16 here is flat root only */
    const char *base = raw;
    for (const char *p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    /* uppercase base into fixed[]; track if extension already present */
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

    /* lowercase base name (sans extension) for child's argv[0] */
    static char prog_name[16];
    int  pn = 0;
    while (pn < (int)sizeof(prog_name) - 1 && base[pn] && base[pn] != '.') {
        char c = base[pn];
        if (c >= 'A' && c <= 'Z') c += 32;
        prog_name[pn] = c;
        pn++;
    }
    prog_name[pn] = 0;

    /* child argv = [prog_name, argv[2], argv[3], ..., NULL] */
    static char *child_argv[16];
    int n = 0;
    child_argv[n++] = prog_name;
    for (int j = 2; j < argc && n < 15; j++) {
        child_argv[n++] = argv[j];
    }
    child_argv[n] = 0;

    long code = exec(fixed, child_argv);
    console_clear();
    printf("[%s exited %d]\n", fixed, (int)code);
}

/* main loop */

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

        if (strcmp(cmd, "ls")    == 0) 					{ printf("running command ls\n"); 				builtin_ls(); 			 					}
        else if (strcmp(cmd, "help")  == 0) 		{ printf("running command help\n"); 			builtin_help();								}
        else if (strcmp(cmd, "clear") == 0) 		{ printf("running command clear\n"); 			builtin_clear();							}
        else if (strcmp(cmd, "rm")    == 0) 		{ printf("running command rm\n"); 				builtin_rm(ac, targs);				}
        else if (strcmp(cmd, "run")   == 0) 		{ printf("running command run\n"); 				builtin_run(ac, targs);				}
        else if (strcmp(cmd, "cat")   == 0) 		{ printf("running command cat\n"); 				builtin_cat(ac, targs); 			}
        else if (strcmp(cmd, "echo")  == 0) 		{ printf("running command echo\n"); 			builtin_echo(ac, targs);			}
        else if (strcmp(cmd, "write") == 0) 		{ printf("running command write\n"); 			builtin_write(ac, targs);			}
        else if (strcmp(cmd, "reboot") == 0) 		{ printf("running command reboot\n"); 		builtin_reboot(ac, targs);		}
        else if (strcmp(cmd, "shutdown") == 0) 	{ printf("running command shutdown\n"); 	builtin_shutdown(ac, targs);	}
        else if (strcmp(cmd, "exit")  == 0) 		{	printf("exitting SHELF"); return 0;																	}
        else 		printf("%s: unknown command, type help\n", cmd);
        // Shutdown, reboot and, exit cause a pagefault
    }
}
