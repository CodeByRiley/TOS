#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

struct kbd_key {
    uint16_t keycode;     // Linux KEY_* (see input/key_codes.h)
    uint8_t  pressed;     // 1 = press, 0 = release
};

struct kbd_event {
    struct kbd_key key;
};

void keyboard_init(void);
int  keyboard_poll(int *pressed, uint16_t *key);

#endif
