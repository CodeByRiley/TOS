/* userspace/games/doom/doomgeneric_kernel.c — DOOM platform glue for TOS.
 *
 * Implements the doomgeneric "platform" entry points (DG_Init,
 * DG_DrawFrame, DG_GetTicksMs, DG_SleepMs, DG_GetKey, DG_SetWindowTitle)
 * against our framebuffer + tick + keyboard syscalls. Replaces the SDL /
 * Win32 / X11 backends shipped in the upstream doomgeneric tree.
 *
 * Run model: maps the framebuffer once at init, centers the 320x200 DOOM
 * canvas inside whatever physical resolution we got, and blits each frame
 * straight to fb_map() pixels. No double-buffering, no WM — DOOM owns the
 * screen exclusively while running.
 */
#include <stdint.h>
#include "doomgeneric/doomgeneric.h"
#include "../../lib/syscall.h"
#include "../../include/key_codes.h"

extern void doomgeneric_Create(int argc, char **argv);
extern void doomgeneric_Tick(void);
extern int  printf(const char *fmt, ...);

static struct fb_info fbi;
static uint32_t      *fb;
static uint32_t       ox, oy;     /* centring offset within the host fb */

/* DOOM single-byte key codes — values mirror doomgeneric/doomkeys.h. */
#define DOOM_KEY_RIGHTARROW  0xae
#define DOOM_KEY_LEFTARROW   0xac
#define DOOM_KEY_UPARROW     0xad
#define DOOM_KEY_DOWNARROW   0xaf
#define DOOM_KEY_ESCAPE      27
#define DOOM_KEY_ENTER       13
#define DOOM_KEY_TAB         9
#define DOOM_KEY_BACKSPACE   0x7f
#define DOOM_KEY_PAUSE       0xff
#define DOOM_KEY_SHIFT       (0x80 + 0x36)
#define DOOM_KEY_CTRL        (0x80 + 0x1d)
#define DOOM_KEY_ALT         (0x80 + 0x38)
#define DOOM_KEY_F(n)        (0x80 + 0x3a + (n))   /* F1=0x3b, F2=0x3c, ... */

/* Map a Linux KEY_* scancode to a DOOM single-byte key code. Returns 0
 * for any unmapped key so the caller can ignore it. */
static unsigned char linux_to_doom(uint16_t k) {
    switch (k) {
        case KEY_ESC:        return DOOM_KEY_ESCAPE;
        case KEY_ENTER:
        case KEY_KPENTER:    return DOOM_KEY_ENTER;
        case KEY_TAB:        return DOOM_KEY_TAB;
        case KEY_BACKSPACE:  return DOOM_KEY_BACKSPACE;
        case KEY_PAUSE:      return DOOM_KEY_PAUSE;
        case KEY_SPACE:      return ' ';
        case KEY_UP:         return DOOM_KEY_UPARROW;
        case KEY_DOWN:       return DOOM_KEY_DOWNARROW;
        case KEY_LEFT:       return DOOM_KEY_LEFTARROW;
        case KEY_RIGHT:      return DOOM_KEY_RIGHTARROW;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return DOOM_KEY_SHIFT;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:  return DOOM_KEY_CTRL;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:   return DOOM_KEY_ALT;
        case KEY_F1:         return DOOM_KEY_F(1);
        case KEY_F2:         return DOOM_KEY_F(2);
        case KEY_F3:         return DOOM_KEY_F(3);
        case KEY_F4:         return DOOM_KEY_F(4);
        case KEY_F5:         return DOOM_KEY_F(5);
        case KEY_F6:         return DOOM_KEY_F(6);
        case KEY_F7:         return DOOM_KEY_F(7);
        case KEY_F8:         return DOOM_KEY_F(8);
        case KEY_F9:         return DOOM_KEY_F(9);
        case KEY_F10:        return DOOM_KEY_F(10);

        case KEY_1:          return '1';
        case KEY_2:          return '2';
        case KEY_3:          return '3';
        case KEY_4:          return '4';
        case KEY_5:          return '5';
        case KEY_6:          return '6';
        case KEY_7:          return '7';
        case KEY_8:          return '8';
        case KEY_9:          return '9';
        case KEY_0:          return '0';
        case KEY_MINUS:      return '-';
        case KEY_EQUAL:      return '=';

        case KEY_A: return 'a'; case KEY_B: return 'b'; case KEY_C: return 'c';
        case KEY_D: return 'd'; case KEY_E: return 'e'; case KEY_F: return 'f';
        case KEY_G: return 'g'; case KEY_H: return 'h'; case KEY_I: return 'i';
        case KEY_J: return 'j'; case KEY_K: return 'k'; case KEY_L: return 'l';
        case KEY_M: return 'm'; case KEY_N: return 'n'; case KEY_O: return 'o';
        case KEY_P: return 'p'; case KEY_Q: return 'q'; case KEY_R: return 'r';
        case KEY_S: return 's'; case KEY_T: return 't'; case KEY_U: return 'u';
        case KEY_V: return 'v'; case KEY_W: return 'w'; case KEY_X: return 'x';
        case KEY_Y: return 'y'; case KEY_Z: return 'z';

        case KEY_COMMA:      return ',';
        case KEY_DOT:        return '.';
        case KEY_SLASH:      return '/';
        case KEY_SEMICOLON:  return ';';
        case KEY_APOSTROPHE: return '\'';
        case KEY_GRAVE:      return '`';
        case KEY_BACKSLASH:  return '\\';
        case KEY_LEFTBRACE:  return '[';
        case KEY_RIGHTBRACE: return ']';

        default:             return 0;
    }
}

/* Snapshot the framebuffer geometry and compute the (ox, oy) origin that
 * centres the 320x200 DOOM canvas inside it. */
void DG_Init(void) {
    fb_info(&fbi);
    fb = (uint32_t*)fb_map();
    ox = (fbi.width  > DOOMGENERIC_RESX) ? (fbi.width  - DOOMGENERIC_RESX) / 2 : 0;
    oy = (fbi.height > DOOMGENERIC_RESY) ? (fbi.height - DOOMGENERIC_RESY) / 2 : 0;
    printf("[DOOM] fb %dx%d, draw region offset=(%d,%d)\n",
           (int)fbi.width, (int)fbi.height, (int)ox, (int)oy);
}

/* Blit DG_ScreenBuffer (320x200 BGRA) to the framebuffer at (ox, oy). */
void DG_DrawFrame(void) {
    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        uint32_t *src = &DG_ScreenBuffer[y * DOOMGENERIC_RESX];
        uint32_t *dst = (uint32_t*)((uint8_t*)fb + (oy + y) * fbi.pitch + ox * 4);
        for (uint32_t x = 0; x < DOOMGENERIC_RESX; x++) dst[x] = src[x];
    }
}

/* Busy-wait `ms` milliseconds against the tick counter. */
void DG_SleepMs(uint32_t ms) {
    long target = get_ticks() + (long)ms;
    while (get_ticks() < target) { }
}

/* Wall-clock-ish millisecond counter (actually uptime). */
uint32_t DG_GetTicksMs(void) {
    return (uint32_t)get_ticks();
}

/* Drain the keyboard ring into DOOM key events. Returns 1 if a key was
 * delivered, 0 if no more events are pending. Unmapped scancodes are
 * logged and skipped. */
int DG_GetKey(int *pressed, unsigned char *doomKey) {
    uint16_t k;
    while (kbd_poll(pressed, &k)) {
        unsigned char dk = linux_to_doom(k);
        if (dk) {
            *doomKey = dk;
            return 1;
        } else {
            printf("unmapped key: %d\n", (int)k);
            continue;
        }
    }
    return 0;
}

/* DOOM expects a window title call; we have no chrome to update. */
void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* Entry point. Hard-codes the DOOM1.WAD path because there's no argv
 * propagation from the shell into ELF children yet. */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char *fake_argv[] = { (char*)"doom", (char*)"-iwad", (char*)"DOOM1.WAD", 0 };
    doomgeneric_Create(3, fake_argv);
    while (1) {
        doomgeneric_Tick();
    }
    return 0;
}
