/* userspace/bin/uidemo/uidemo.c , live reference for lib/gfx and lib/ui.
 *
 * A libwm client that draws every widget lib/ui offers and reacts to the
 * pointer, so the API can be seen working rather than only read about.
 * It is also the smallest complete example of the shape a wm client
 * takes: create a window, drain events into local state, redraw the whole
 * surface from that state, invalidate, yield.
 *
 * Because lib/ui is immediate mode, there is no widget setup and no
 * teardown , the window's appearance is a pure function of the handful of
 * variables in main(), recomputed every frame.
 */
#include <lib/syscall.h>
#include <lib/wm.h>
#include <lib/gfx.h>
#include <lib/ui.h>
#include <include/key_codes.h>

extern int printf(const char *, ...);
extern void *memset(void *, int, size_t);

#define WIN_W 420
#define WIN_H 300

#define ROW_H     20
#define ROW_GAP   4
#define MARGIN    8

/* Tiny integer-to-string; the demo has no need for printf into a buffer
 * and pulling one in would obscure how little this file requires. */
static void int_to_str(int v, char *out, int max) {
    char tmp[12];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;

    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 11);
    if (neg && n < 11) tmp[n++] = '-';

    int i = 0;
    while (n > 0 && i < max - 1) out[i++] = tmp[--n];
    out[i] = 0;
}

static void label_with_number(char *buf, int max, const char *prefix, int v) {
    int i = 0;
    while (prefix[i] && i < max - 1) { buf[i] = prefix[i]; i++; }
    buf[i] = 0;
    if (i < max - 1) int_to_str(v, buf + i, max - i);
}

/* A patch of lib/gfx drawing that is not a widget, to show the layer
 * underneath ui is available directly. */
static void draw_swatches(struct gfx_surface *s, struct gfx_rect r,
                          int grid) {
    struct gfx_rect prev = gfx_clip_push(s, r);

    static const uint32_t colors[6] = {
        0x00C04040u, 0x00C08040u, 0x00C0C040u,
        0x0040C040u, 0x004080C0u, 0x008040C0u,
    };
    for (int i = 0; i < 6; i++) {
        struct gfx_rect c = ui_layout_column(r, 6, i, 2);
        c.y += 2;
        c.h -= 4;
        gfx_fill(s, c, colors[i]);
        /* Alpha over the lower half: the same colour composited at 50%
         * shows the blend path is doing something the fill is not. */
        struct gfx_rect half = gfx_rect_make(c.x, c.y + c.h / 2,
                                             c.w, c.h - c.h / 2);
        gfx_fill_blend(s, half, 0x80FFFFFFu);
    }

    if (grid) {
        for (int x = r.x; x < r.x + r.w; x += 8)
            gfx_vline(s, x, r.y, r.h, 0x00FFFFFFu);
        for (int y = r.y; y < r.y + r.h; y += 8)
            gfx_hline(s, r.x, y, r.w, 0x00FFFFFFu);
    }

    gfx_clip_set(s, prev);
}

int main(void) {
    struct wm_window win;
    if (wm_window_create(WIN_W, WIN_H, "uidemo", &win) != 0) {
        printf("uidemo: wm_window_create failed\n");
        return 1;
    }
    printf("uidemo: window %d %dx%d\n", win.handle, win.w, win.h);

    struct gfx_surface surface;
    gfx_surface_init(&surface, (uint32_t *)(uintptr_t)win.surface_va,
                     win.w, win.h, (int)(win.pitch / 4));

    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));

    int mouse_x = 0, mouse_y = 0;
    int buttons = 0;
    int count = 0;
    int auto_run = 0;
    int show_grid = 0;
    int clicks = 0;

    for (;;) {
        /* Drain events, but stop after one button transition so a press
         * and the release that follows it never collapse into the same
         * frame. lib/ui detects clicks on the down-then-up edge, and a
         * frame that saw neither would swallow the click entirely. */
        int button_edges = 0;
        struct wm_event ev;
        while (button_edges < 1 && wm_poll_event(&ev)) {
            switch (ev.type) {
            case WM_EV_KEY_DOWN:
                if (ev.param == KEY_ESC) {
                    wm_window_destroy(win.handle);
                    return 0;
                }
                break;

            case WM_EV_MOUSE_MOVE:
                mouse_x = ev.x;
                mouse_y = ev.y;
                break;

            case WM_EV_MOUSE_DOWN:
                mouse_x = ev.x;
                mouse_y = ev.y;
                buttons = ev.param;
                button_edges++;
                break;

            case WM_EV_MOUSE_UP:
                mouse_x = ev.x;
                mouse_y = ev.y;
                buttons &= ~ev.param;
                button_edges++;
                break;

            case WM_EV_RESIZE:
                /* The old surface va dies with the old backing. */
                win.surface_va = ev.surface_va;
                win.pitch      = ev.pitch;
                win.w          = ev.w;
                win.h          = ev.h;
                gfx_surface_init(&surface,
                                 (uint32_t *)(uintptr_t)win.surface_va,
                                 win.w, win.h, (int)(win.pitch / 4));
                break;

            case WM_EV_QUIT:
                wm_window_destroy(win.handle);
                return 0;

            default:
                break;
            }
        }

        if (auto_run) count = (count + 1) % 101;

        /* ---- one frame ------------------------------------------------ */
        ui_begin(&ui, &surface, 0, mouse_x, mouse_y, buttons);

        struct gfx_rect bounds = gfx_surface_bounds(&surface);
        ui_panel(&ui, bounds);

        struct ui_layout col;
        ui_layout_begin(&col, gfx_rect_inset(bounds, MARGIN), ROW_GAP);

        ui_label(&ui, ui_layout_row(&col, ROW_H),
                 "lib/ui - immediate mode widgets");
        ui_separator(&ui, ui_layout_row(&col, 4));

        /* Buttons in a three-column row. */
        struct gfx_rect row = ui_layout_row(&col, ROW_H + 4);
        if (ui_button(&ui, ui_layout_column(row, 3, 0, ROW_GAP), "-10")) {
            count -= 10;
            if (count < 0) count = 0;
            clicks++;
        }
        if (ui_button(&ui, ui_layout_column(row, 3, 1, ROW_GAP), "+10")) {
            count += 10;
            if (count > 100) count = 100;
            clicks++;
        }
        if (ui_button(&ui, ui_layout_column(row, 3, 2, ROW_GAP), "Reset")) {
            count = 0;
            clicks++;
        }

        char buf[48];
        label_with_number(buf, sizeof(buf), "count: ", count);
        ui_progress(&ui, ui_layout_row(&col, ROW_H), count, buf);
        if (ui_slider(&ui, ui_layout_row(&col, ROW_H), 0, 100, &count))
            clicks++;

        ui_checkbox(&ui, ui_layout_row(&col, ROW_H), "auto advance",
                    &auto_run);
        ui_checkbox(&ui, ui_layout_row(&col, ROW_H), "grid overlay",
                    &show_grid);

        ui_separator(&ui, ui_layout_row(&col, 4));

        /* A well with raw lib/gfx drawing inside it. */
        struct gfx_rect well = ui_layout_row(&col, 44);
        ui_well(&ui, well);
        draw_swatches(&surface, gfx_rect_inset(well, 2), show_grid);

        /* Status line, so pointer routing is visible. */
        label_with_number(buf, sizeof(buf), "clicks: ", clicks);
        struct gfx_rect status = ui_layout_row(&col, ROW_H);
        ui_label(&ui, status, buf);

        label_with_number(buf, sizeof(buf), "x=", mouse_x);
        int n = 0; while (buf[n]) n++;
        label_with_number(buf + n, (int)sizeof(buf) - n, "  y=", mouse_y);
        ui_label_muted(&ui, ui_layout_column(status, 2, 1, 0), buf);

        ui_label_muted(&ui, ui_layout_row(&col, ROW_H), "ESC to quit");

        ui_end(&ui);

        wm_window_invalidate(win.handle);
        yield();
    }
}
