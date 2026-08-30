/* kernel/input/keyboard.h , PS/2 keyboard driver surface.
 *
 * Translates IRQ1 scancodes into Linux KEY_* codes (see input/key_codes.h)
 * and exposes a polling reader for the syscall layer. Press/release state
 * is delivered explicitly so userspace can track modifier holds.
 *
 * Implementation: kernel/input/keyboard.c.
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <utilities/types.h>

struct kbd_key {
    u16 keycode;     /* Linux KEY_* (see input/key_codes.h) */
    u8  pressed;     /* 1 = press, 0 = release              */
};

struct kbd_event {
    struct kbd_key key;
};

void keyboard_init(void);

/* Pop one event into *pressed / *key. Returns 1 on success, 0 if ring
 * empty. Lossy on overflow , sticky modifier holds may desync. */
int  keyboard_poll(int *pressed, u16 *key);

#endif
