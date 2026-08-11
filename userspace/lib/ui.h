/* userspace/lib/ui.h — immediate-mode widgets over lib/gfx.
 *
 * No retained widget tree, no allocation, no callbacks: each frame the
 * caller redraws by calling ui_button() and friends, and a widget reports
 * what happened by its return value. That suits a poll loop — the shape
 * every wm client already has — and it means a window's appearance is
 * always a pure function of the caller's own state.
 *
 * Typical frame:
 *
 *     ui_begin(&ui, &surface, 0, mouse_x, mouse_y, buttons);
 *     ui_panel(&ui, gfx_rect_make(0, 0, w, h));
 *
 *     struct ui_layout col;
 *     ui_layout_begin(&col, gfx_rect_inset(bounds, 8), 4);
 *     if (ui_button(&ui, ui_layout_row(&col, 20), "Start")) start();
 *     ui_checkbox(&ui, ui_layout_row(&col, 16), "Loop", &looping);
 *     ui_end(&ui);
 *
 * Widgets are identified by call order within a frame, so the sequence of
 * calls must be stable between frames. Branching around a widget shifts
 * every later id by one; keep conditional widgets at the end, or draw
 * them disabled instead of skipping them.
 *
 * Implementation: userspace/lib/ui.c.
 */
#ifndef UI_H
#define UI_H

#include "gfx.h"

/* ---------------- Theme --------------------------------------------------- */

struct ui_theme {
    uint32_t face;          /* button and panel fill                      */
    uint32_t face_hover;
    uint32_t face_active;   /* held down                                  */
    uint32_t text;
    uint32_t text_muted;    /* disabled labels                            */
    uint32_t light;         /* bevel highlight                            */
    uint32_t dark;          /* bevel shadow                               */
    uint32_t accent;        /* progress fill, checkbox tick               */
    uint32_t accent_text;
    int      pad;           /* inner padding, pixels                      */
    int      border;        /* bevel thickness                            */
    int      scale;         /* text magnification                         */
};

/* Grey bevels and navy accents — the palette winman's chrome already
 * uses, so widgets sit next to it without clashing. */
extern const struct ui_theme ui_theme_default;

/* ---------------- Context -------------------------------------------------- */

struct ui_context {
    struct gfx_surface    *s;
    const struct ui_theme *theme;

    int mx, my;             /* pointer, in surface coordinates            */
    int down;               /* primary button held this frame             */
    int was_down;           /* ...and last frame, for edge detection      */

    int hot;                /* widget under the pointer                   */
    int active;             /* widget that took the press                 */
    int next_id;
};

/* Start a frame. `buttons` is a MOUSE_BTN_* mask; only the left button
 * drives widget state. Passing theme = NULL uses ui_theme_default.
 * Pointer coordinates are relative to the surface, so a wm client passes
 * event x/y straight through. */
void ui_begin(struct ui_context *c, struct gfx_surface *s,
              const struct ui_theme *theme,
              int mouse_x, int mouse_y, int buttons);

/* Finish a frame. Releases the active widget once the button comes up, so
 * a press that ends outside its widget cancels rather than clicking. */
void ui_end(struct ui_context *c);

/* ---------------- Layout ---------------------------------------------------- */

/* A vertical stack. ui_layout_row hands out full-width bands top to
 * bottom; when the area is used up it returns empty rects, which every
 * widget draws as nothing. */
struct ui_layout {
    struct gfx_rect area;
    int             y;
    int             gap;
};

void            ui_layout_begin(struct ui_layout *l, struct gfx_rect area,
                                int gap);
struct gfx_rect ui_layout_row(struct ui_layout *l, int h);

/* Divide a row into `n` equal columns and return column `i`. */
struct gfx_rect ui_layout_column(struct gfx_rect row, int n, int i, int gap);

/* ---------------- Widgets ---------------------------------------------------- */

/* Beveled background panel. Draw before the widgets that sit on it. */
void ui_panel(struct ui_context *c, struct gfx_rect r);

/* Sunken area, for content wells and text fields. */
void ui_well(struct ui_context *c, struct gfx_rect r);

void ui_label(struct ui_context *c, struct gfx_rect r, const char *text);
void ui_label_centered(struct ui_context *c, struct gfx_rect r,
                       const char *text);
void ui_label_muted(struct ui_context *c, struct gfx_rect r,
                    const char *text);

/* Returns 1 on the frame the button is released with the pointer still
 * inside it. A press that drags off and releases elsewhere returns 0. */
int  ui_button(struct ui_context *c, struct gfx_rect r, const char *label);

/* Toggles *value and returns 1 on the frame it changed. */
int  ui_checkbox(struct ui_context *c, struct gfx_rect r,
                 const char *label, int *value);

/* Fills `r` proportionally; percent is clamped to 0..100. When `label` is
 * non-NULL it is centred over the bar. */
void ui_progress(struct ui_context *c, struct gfx_rect r, int percent,
                 const char *label);

/* Horizontal rule centred in `r`. */
void ui_separator(struct ui_context *c, struct gfx_rect r);

#endif
