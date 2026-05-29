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

/* Pop one event into *out. Returns 1 if got an event, 0 if ring empty. */
int  mouse_poll(struct mouse_event *out);

/* Absolute cursor x/y maintained by the driver. Clamped to [0, w-1] /
 * [0, h-1] where w/h come from mouse_set_bounds (default: unbounded). */
int32_t mouse_x(void);
int32_t mouse_y(void);
uint8_t mouse_buttons(void);

void mouse_set_bounds(int32_t w, int32_t h);

#endif
