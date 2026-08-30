#include "app.h"

#include <lib/app_info.h>
#include <string.h>

const struct app_info *app_self_info(void) {
    for (const struct app_info *p = __appinfo_start; p < __appinfo_end; p++)
        if (p->magic == APP_INFO_MAGIC)
            return p;
    return 0;   /* binary carries no APP_INFO */
}

static void bind_surface(struct app *app) {
    gfx_surface_init(&app->surface,
                     (uint32_t *)(uintptr_t)app->window.surface_va,
                     app->window.w, app->window.h,
                     (int)(app->window.pitch / sizeof(uint32_t)));
}

int app_open(struct app *app, int width, int height, const char *title) {
    return app_open_ex(app, width, height, title, 0);
}

int app_open_ex(struct app *app, int width, int height, const char *title,
                uint32_t window_flags) {
    if (!app)
        return -1;
    memset(app, 0, sizeof(*app));
    if (wm_window_create_ex(width, height, title, window_flags,
                            &app->window) != 0)
        return -1;
    bind_surface(app);
    app->open = 1;
    app->redraw_requested = 1;
    return 0;
}

void app_close(struct app *app) {
    if (!app || !app->open)
        return;
    wm_window_destroy(app->window.handle);
    memset(&app->window, 0, sizeof(app->window));
    memset(&app->surface, 0, sizeof(app->surface));
    app->open = 0;
    app->redraw_requested = 0;
}

int app_poll_event(struct app *app, struct wm_event *event) {
    if (!app || !app->open || !event)
        return 0;
    if (!wm_poll_event(event))
        return 0;

    if (event->type == WM_EV_RESIZE) {
        app->window.surface_va = event->surface_va;
        app->window.pitch = event->pitch;
        app->window.w = event->w;
        app->window.h = event->h;
        bind_surface(app);
        app->redraw_requested = 1;
    }
    return 1;
}

void app_request_redraw(struct app *app) {
    if (app && app->open)
        app->redraw_requested = 1;
}

int app_needs_redraw(const struct app *app) {
    return app && app->open && app->redraw_requested;
}

int app_present(struct app *app) {
    if (!app || !app->open)
        return -1;
    int result = wm_window_invalidate(app->window.handle);
    if (result == 0)
        app->redraw_requested = 0;
    return result;
}

static int apply_action(struct app *app, int action) {
    if (action & APP_ACTION_REDRAW)
        app_request_redraw(app);
    return (action & APP_ACTION_EXIT) != 0;
}

static int draw_if_needed(struct app *app,
                          const struct app_callbacks *callbacks,
                          void *context) {
    if (!app_needs_redraw(app) || !callbacks || !callbacks->draw)
        return 0;
    callbacks->draw(app, context);
    app_present(app);
    return 1;
}

int app_run(struct app *app, const struct app_callbacks *callbacks,
            void *context) {
    if (!app || !app->open)
        return -1;

    int exit_requested = 0;
    while (app->open && !exit_requested) {
        struct wm_event event;
        if (app_poll_event(app, &event)) {
            int action = APP_ACTION_NONE;
            if (callbacks && callbacks->event)
                action = callbacks->event(app, &event, context);
            if (event.type == WM_EV_QUIT && action == APP_ACTION_NONE)
                action = APP_ACTION_EXIT;   /* NONE on QUIT = no objection */
            exit_requested = apply_action(app, action);
            draw_if_needed(app, callbacks, context);
            continue;
        }

        int worked = draw_if_needed(app, callbacks, context);
        if (!worked && callbacks && callbacks->idle) {
            int action = callbacks->idle(app, context);
            exit_requested = apply_action(app, action);
            worked = draw_if_needed(app, callbacks, context);
            if (worked)
                sleep_ticks(1);   /* idle frames are tick-paced */
        }
        if (!worked && !exit_requested)
            sleep_ticks(1);
    }

    app_close(app);
    return 0;
}
