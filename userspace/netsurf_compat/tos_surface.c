#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <include/key_codes.h>
#include <lib/keymap.h>
#include <lib/syscall.h>
#include <lib/wm.h>

#include <libnsfb.h>
#include <libnsfb_event.h>
#include <libnsfb_plot.h>
#include <libnsfb_plot_util.h>

#include "cursor.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"

struct tos_surface {
    struct wm_window window;
};

static int tos_defaults(nsfb_t *nsfb)
{
    nsfb->width = 800;
    nsfb->height = 600;
    nsfb->format = NSFB_FMT_XRGB8888;
    return select_plotters(nsfb) ? 0 : -1;
}

static int tos_geometry(nsfb_t *nsfb, int width, int height,
                        enum nsfb_format_e format)
{
    if (format != NSFB_FMT_XRGB8888) {
        return -1;
    }

    nsfb->width = width;
    nsfb->height = height;
    nsfb->format = format;
    struct tos_surface *surface = nsfb->surface_priv;
    nsfb->linelen = surface == NULL ? width * 4 : (int)surface->window.pitch;
    return select_plotters(nsfb) ? 0 : -1;
}

static int tos_initialise(nsfb_t *nsfb)
{
    struct tos_surface *surface = calloc(1, sizeof(*surface));
    if (surface == NULL) {
        return -1;
    }

    if (wm_window_create(nsfb->width, nsfb->height, "NetSurf",
                         &surface->window) != 0) {
        free(surface);
        return -1;
    }

    nsfb->surface_priv = surface;
    nsfb->ptr = (uint8_t *)(uintptr_t)surface->window.surface_va;
    nsfb->linelen = (int)surface->window.pitch;
    return 0;
}

static int tos_finalise(nsfb_t *nsfb)
{
    struct tos_surface *surface = nsfb->surface_priv;
    if (surface != NULL) {
        wm_window_destroy(surface->window.handle);
        free(surface);
    }
    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    return 0;
}

static enum nsfb_key_code_e tos_keycode(int key)
{
    char ascii = keymap_to_ascii((uint16_t)key, 0);
    if (ascii != 0) {
        return (enum nsfb_key_code_e)ascii;
    }

    switch (key) {
    case KEY_ESC: return NSFB_KEY_ESCAPE;
    case KEY_DELETE: return NSFB_KEY_DELETE;
    case KEY_INSERT: return NSFB_KEY_INSERT;
    case KEY_HOME: return NSFB_KEY_HOME;
    case KEY_END: return NSFB_KEY_END;
    case KEY_PAGEUP: return NSFB_KEY_PAGEUP;
    case KEY_PAGEDOWN: return NSFB_KEY_PAGEDOWN;
    case KEY_UP: return NSFB_KEY_UP;
    case KEY_DOWN: return NSFB_KEY_DOWN;
    case KEY_LEFT: return NSFB_KEY_LEFT;
    case KEY_RIGHT: return NSFB_KEY_RIGHT;
    case KEY_F1: return NSFB_KEY_F1;
    case KEY_F2: return NSFB_KEY_F2;
    case KEY_F3: return NSFB_KEY_F3;
    case KEY_F4: return NSFB_KEY_F4;
    case KEY_F5: return NSFB_KEY_F5;
    case KEY_F6: return NSFB_KEY_F6;
    case KEY_F7: return NSFB_KEY_F7;
    case KEY_F8: return NSFB_KEY_F8;
    case KEY_F9: return NSFB_KEY_F9;
    case KEY_F10: return NSFB_KEY_F10;
    case KEY_F11: return NSFB_KEY_F11;
    case KEY_F12: return NSFB_KEY_F12;
    case KEY_LEFTSHIFT: return NSFB_KEY_LSHIFT;
    case KEY_RIGHTSHIFT: return NSFB_KEY_RSHIFT;
    case KEY_LEFTCTRL: return NSFB_KEY_LCTRL;
    case KEY_RIGHTCTRL: return NSFB_KEY_RCTRL;
    case KEY_LEFTALT: return NSFB_KEY_LALT;
    case KEY_RIGHTALT: return NSFB_KEY_RALT;
    case KEY_LEFTMETA: return NSFB_KEY_LMETA;
    case KEY_RIGHTMETA: return NSFB_KEY_RMETA;
    case KEY_MENU: return NSFB_KEY_MENU;
    case KEY_CAPSLOCK: return NSFB_KEY_CAPSLOCK;
    case KEY_NUMLOCK: return NSFB_KEY_NUMLOCK;
    case KEY_SCROLLLOCK: return NSFB_KEY_SCROLLOCK;
    case KEY_PAUSE: return NSFB_KEY_PAUSE;
    case KEY_SYSRQ: return NSFB_KEY_SYSREQ;
    default: return NSFB_KEY_UNKNOWN;
    }
}

static enum nsfb_key_code_e tos_mouse_button(int buttons)
{
    if (buttons & MOUSE_BTN_LEFT) return NSFB_KEY_MOUSE_1;
    if (buttons & MOUSE_BTN_MIDDLE) return NSFB_KEY_MOUSE_2;
    if (buttons & MOUSE_BTN_RIGHT) return NSFB_KEY_MOUSE_3;
    if (buttons & MOUSE_BTN_FORWARD) return NSFB_KEY_MOUSE_4;
    if (buttons & MOUSE_BTN_BACK) return NSFB_KEY_MOUSE_5;
    return NSFB_KEY_UNKNOWN;
}

static bool tos_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct tos_surface *surface = nsfb->surface_priv;
    struct wm_event input;
    (void)timeout;

    if (surface == NULL || !wm_poll_event(&input)) {
        yield();
        return false;
    }

    switch (input.type) {
    case WM_EV_KEY_DOWN:
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = tos_keycode(input.param);
        return true;
    case WM_EV_KEY_UP:
        event->type = NSFB_EVENT_KEY_UP;
        event->value.keycode = tos_keycode(input.param);
        return true;
    case WM_EV_MOUSE_MOVE:
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = input.x;
        event->value.vector.y = input.y;
        event->value.vector.z = 0;
        return true;
    case WM_EV_MOUSE_DOWN:
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = tos_mouse_button(input.param);
        return true;
    case WM_EV_MOUSE_UP:
        event->type = NSFB_EVENT_KEY_UP;
        event->value.keycode = tos_mouse_button(input.param);
        return true;
    case WM_EV_RESIZE:
        surface->window.surface_va = input.surface_va;
        surface->window.pitch = input.pitch;
        surface->window.w = input.w;
        surface->window.h = input.h;
        nsfb->ptr = (uint8_t *)(uintptr_t)input.surface_va;
        nsfb->linelen = (int)input.pitch;
        nsfb->width = input.w;
        nsfb->height = input.h;
        if (nsfb->cursor != NULL) {
            nsfb->cursor->plotted = false;
        }
        event->type = NSFB_EVENT_RESIZE;
        event->value.resize.w = input.w;
        event->value.resize.h = input.h;
        return true;
    case WM_EV_QUIT:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;
    default:
        return false;
    }
}

static int tos_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor != NULL && cursor->plotted &&
        nsfb_plot_bbox_intersect(box, &cursor->loc)) {
        nsfb_cursor_clear(nsfb, cursor);
    }
    return 0;
}

static int tos_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct tos_surface *surface = nsfb->surface_priv;
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    (void)box;

    if (cursor != NULL && !cursor->plotted) {
        nsfb_cursor_plot(nsfb, cursor);
    }
    return surface == NULL ? -1 : wm_window_invalidate(surface->window.handle);
}

static int tos_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    struct tos_surface *surface = nsfb->surface_priv;
    if (surface == NULL || cursor == NULL || !cursor->plotted) {
        return 0;
    }

    nsfb_cursor_clear(nsfb, cursor);
    nsfb_cursor_plot(nsfb, cursor);
    return wm_window_invalidate(surface->window.handle);
}

static const nsfb_surface_rtns_t tos_rtns = {
    .defaults = tos_defaults,
    .initialise = tos_initialise,
    .finalise = tos_finalise,
    .geometry = tos_geometry,
    .input = tos_input,
    .claim = tos_claim,
    .update = tos_update,
    .cursor = tos_cursor,
};

NSFB_SURFACE_DEF(tos, NSFB_SURFACE_SDL, &tos_rtns)
