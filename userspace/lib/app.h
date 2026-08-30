/* One-window application runtime over libwm and libgfx. */
#ifndef TOS_APP_H
#define TOS_APP_H

#include <lib/gfx.h>
#include <lib/wm.h>
#include <stdint.h>

struct app {
    struct wm_window window;
    struct gfx_surface surface;
    int open;
    int redraw_requested;
};

enum app_action {
    APP_ACTION_NONE   = 0,
    APP_ACTION_REDRAW = 1,
    APP_ACTION_EXIT   = 2,
};

struct app_callbacks {
    /* Called once per event. Returning REDRAW causes a draw before the next
     * event is consumed, preserving immediate-mode button edges. */
    int (*event)(struct app *app, const struct wm_event *event, void *context);
    void (*draw)(struct app *app, void *context);
    /* Called when no event was ready. Useful for animation/timers. */
    int (*idle)(struct app *app, void *context);
};

int app_open(struct app *app, int width, int height, const char *title);
int app_open_ex(struct app *app, int width, int height, const char *title,
                uint32_t window_flags);
void app_close(struct app *app);

/* Poll one WM event and automatically replace the gfx surface after resize. */
int app_poll_event(struct app *app, struct wm_event *event);

void app_request_redraw(struct app *app);
int app_needs_redraw(const struct app *app);
int app_present(struct app *app);

/* Run until a callback returns APP_ACTION_EXIT. With no event callback,
 * WM_EV_QUIT exits by default. The window is closed before returning. */
int app_run(struct app *app, const struct app_callbacks *callbacks,
            void *context);

#endif
