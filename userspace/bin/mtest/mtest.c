/* userspace/bin/mtest/mtest.c — libwm + mouse smoke test.
 *
 * Spawns a 640x480 window, draws a cursor at the last-known mouse
 * position, drops a colored dot on every MOUSE_DOWN, and quits on ESC.
 * Exercises window create/poll/invalidate, multi-button mouse state, and
 * the WM_EV_RESIZE surface-rebind path (grip-drag → fresh shared buffer).
 */
#include <lib/syscall.h>
#include <lib/wm.h>
#include <include/key_codes.h>

extern int printf(const char *, ...);

#define CURSOR_SIZE   12
#define CURSOR_COLOR  0x00FFFFFFu
#define BG_COLOR      0x00202040u
#define HIT_COLOR     0x00FF4040u

#define MAX_DOTS 256
static struct { int16_t x, y; uint32_t color; } dots[MAX_DOTS];
static int dot_count = 0;

/* Fill a w x h rect into the window surface. The shared surface is a
 * tight w*h*4 BGRA buffer, so row stride is implicitly fb_w pixels. */
static void fill_rect(uint32_t *px, int fb_w, int fb_h,
                      int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = px + (size_t)(y + yy) * (size_t)fb_w;
        for (int xx = 0; xx < w; xx++) row[x + xx] = color;
    }
}

/* Flat-fill the entire surface. */
static void clear_surface(uint32_t *px, int fb_w, int fb_h, uint32_t color) {
    size_t n = (size_t)fb_w * (size_t)fb_h;
    for (size_t i = 0; i < n; i++) px[i] = color;
}

/* Repaint background → dot trail → cursor (top-most). */
static void redraw(uint32_t *px, int fb_w, int fb_h,
                   int cur_x, int cur_y, uint8_t buttons) {
    clear_surface(px, fb_w, fb_h, BG_COLOR);
    for (int i = 0; i < dot_count; i++) {
        fill_rect(px, fb_w, fb_h,
                  dots[i].x - 2, dots[i].y - 2, 5, 5, dots[i].color);
    }
    uint32_t color = buttons ? HIT_COLOR : CURSOR_COLOR;
    fill_rect(px, fb_w, fb_h,
              cur_x - CURSOR_SIZE / 2, cur_y - CURSOR_SIZE / 2,
              CURSOR_SIZE, CURSOR_SIZE, color);
}

int main(void) {
    struct wm_window win;
    if (wm_window_create(640, 480, "mtest", &win) != 0) {
        printf("mtest: wm_window_create failed\n");
        return 1;
    }
    printf("mtest: window %d %dx%d va=%lx\n",
           win.handle, win.w, win.h, (unsigned long)win.surface_va);

    uint32_t *px = (uint32_t *)(uintptr_t)win.surface_va;
    int cur_x = win.w / 2;
    int cur_y = win.h / 2;
    uint8_t buttons = 0;
    int dirty = 1;

    /* Event loop: drain all pending events, then composite + yield if
     * anything changed. ev.param on mouse events is the held-button mask;
     * we mirror it locally so we can color the cursor while held. */
    for (;;) {
        struct wm_event ev;
        while (wm_poll_event(&ev)) {
            switch (ev.type) {
            case WM_EV_KEY_DOWN:
                if (ev.param == KEY_ESC) {
                    printf("mtest: ESC, exiting\n");
                    wm_window_destroy(win.handle);
                    return 0;
                }
                break;

            case WM_EV_MOUSE_MOVE:
                cur_x = ev.x;
                cur_y = ev.y;
                dirty = 1;
                break;

            case WM_EV_MOUSE_DOWN:
                cur_x   = ev.x;
                cur_y   = ev.y;
                buttons = (uint8_t)ev.param;
                if (dot_count < MAX_DOTS) {
                    uint32_t c = 0x00808080u;
                    if (ev.param & MOUSE_BTN_LEFT)   c = 0x000080FFu;
                    if (ev.param & MOUSE_BTN_RIGHT)  c = 0x00FFB000u;
                    if (ev.param & MOUSE_BTN_MIDDLE) c = 0x0080FF80u;
                    dots[dot_count].x     = (int16_t)ev.x;
                    dots[dot_count].y     = (int16_t)ev.y;
                    dots[dot_count].color = c;
                    dot_count++;
                }
                dirty = 1;
                break;

            case WM_EV_MOUSE_UP:
                cur_x   = ev.x;
                cur_y   = ev.y;
                /* Drop released buttons from the held set. */
                buttons &= (uint8_t)~ev.param;
                dirty = 1;
                break;

            case WM_EV_RESIZE:
                /* winman handed us a fresh shared surface; the previous va
                 * becomes invalid the moment winman frees the old backing. */
                px    = (uint32_t *)(uintptr_t)ev.surface_va;
                win.w = ev.w;
                win.h = ev.h;
                win.surface_va = ev.surface_va;
                win.pitch      = ev.pitch;
                /* Keep cursor inside the new bounds. */
                if (cur_x >= win.w) cur_x = win.w - 1;
                if (cur_y >= win.h) cur_y = win.h - 1;
                dirty = 1;
                break;

            case WM_EV_QUIT:
                wm_window_destroy(win.handle);
                return 0;

            default:
                break;
            }
        }

        if (dirty) {
            redraw(px, win.w, win.h, cur_x, cur_y, buttons);
            wm_window_invalidate(win.handle);
            dirty = 0;
        }
        yield();
    }
}
