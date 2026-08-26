/* kernel/input/mouse.h , mouse input surface.
 *
 * Reports relative motion + button mask from hardware mouse drivers. The
 * driver also maintains an absolute cursor position clamped by
 * mouse_set_bounds so apps can read mouse_x()/mouse_y() directly.
 *
 * Implementation: kernel/input/mouse.c.
 */
#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

struct mouse_event {
    int16_t dx;            /* relative since last event; +x = right    */
    int16_t dy;            /* relative since last event; +y = down     */
    uint8_t buttons;       /* MOUSE_BTN_* mask of currently-held btns  */
    uint8_t _pad;
};

void mouse_init(void);

/* Feed one USB HID boot-protocol mouse report: buttons, signed X, signed Y.
 * Extra bytes such as wheel data are ignored for now. */
void mouse_hid_report(const uint8_t *report, uint16_t len);

/* Feed a HID absolute tablet report: buttons, 16-bit X, 16-bit Y. */
void mouse_hid_tablet_report(const uint8_t *report, uint16_t len);

/* Pop one event into *out. Returns 1 on success, 0 if ring empty. */
int  mouse_poll(struct mouse_event *out);

/* Absolute cursor coordinates, clamped to [0, w-1] / [0, h-1] where w/h
 * come from mouse_set_bounds (default: unbounded). */
int32_t mouse_x(void);
int32_t mouse_y(void);
uint8_t mouse_buttons(void);

/* Set the clamp bounds (called when the framebuffer is resized). */
void mouse_set_bounds(int32_t w, int32_t h);

#endif
