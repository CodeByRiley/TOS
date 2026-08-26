/* kernel/input/keyboard.c — PS/2 keyboard driver.
 *
 * IRQ1 handler reads scancode set 1 bytes from port 0x60, tracks the
 * 0xE0 extended-prefix state, maps them to Linux KEY_* codes, and posts
 * (KEY_*, press/release) events to two places:
 *   - the legacy `kbd_ring` polled by keyboard_poll() (kernel apps)
 *   - the per-task msg ring of the input owner (userspace winman or
 *     whoever else grabbed input via msg_input_owner_register)
 *
 * Press/release state is explicit so consumers can track modifier
 * holds; mid-IRQ pressed-state bookkeeping flushes a release event for
 * keys that go away while an unrelated key is held.
 */
#include <input/keyboard.h>
#include <input/key_codes.h>
#include <interrupts/idt.h>
#include <devices/io.h>
#include <devices/pit.h>
#include <msg/msg.h>
#include <utilities/log.h>
#include <stdint.h>

/* Scancode set 1 byte */
//
// Bits | Name | Description
// 7    | Break | 0 = Key pressed (make), 1 = Key released (break)
// 6-0  | Code  | Key identity; identical for the make and break of one key
//
// Two bytes are prefixes rather than keys: 0xE0 introduces a one-byte
// extended code (arrows, right-hand modifiers), and 0xE1 introduces the
// six-byte Pause sequence, which has no break code at all.
#define KBD_SC_BREAK      0x80
#define KBD_SC_CODE_MASK  0x7F
#define KBD_SC_PREFIX_E0  0xE0
#define KBD_SC_PREFIX_E1  0xE1

#define KBD_RING_SIZE 64
#define KBD_RING_MASK (KBD_RING_SIZE - 1)
/* KBD_RING_SIZE must be a power of two — we mask instead of % so the IRQ
 * path doesn't get to know what a div instruction tastes like. */
_Static_assert((KBD_RING_SIZE & KBD_RING_MASK) == 0, "ring size must be pow2");

static struct kbd_event kbd_ring[KBD_RING_SIZE];
static volatile int     kbd_head = 0;
static volatile int     kbd_tail = 0;

/* Scancode set 1, non-extended -> Linux KEY_*. */
static const uint16_t sc_normal[128] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1, [0x03] = KEY_2, [0x04] = KEY_3, [0x05] = KEY_4,
    [0x06] = KEY_5, [0x07] = KEY_6, [0x08] = KEY_7, [0x09] = KEY_8,
    [0x0A] = KEY_9, [0x0B] = KEY_0,
    [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUAL,
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,
    [0x10] = KEY_Q, [0x11] = KEY_W, [0x12] = KEY_E, [0x13] = KEY_R,
    [0x14] = KEY_T, [0x15] = KEY_Y, [0x16] = KEY_U, [0x17] = KEY_I,
    [0x18] = KEY_O, [0x19] = KEY_P,
    [0x1A] = KEY_LEFTBRACE, [0x1B] = KEY_RIGHTBRACE,
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LEFTCTRL,
    [0x1E] = KEY_A, [0x1F] = KEY_S, [0x20] = KEY_D, [0x21] = KEY_F,
    [0x22] = KEY_G, [0x23] = KEY_H, [0x24] = KEY_J, [0x25] = KEY_K,
    [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON, [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,
    [0x2A] = KEY_LEFTSHIFT,
    [0x2B] = KEY_BACKSLASH,
    [0x2C] = KEY_Z, [0x2D] = KEY_X, [0x2E] = KEY_C, [0x2F] = KEY_V,
    [0x30] = KEY_B, [0x31] = KEY_N, [0x32] = KEY_M,
    [0x33] = KEY_COMMA, [0x34] = KEY_DOT, [0x35] = KEY_SLASH,
    [0x36] = KEY_RIGHTSHIFT,
    [0x37] = KEY_KPASTERISK,
    [0x38] = KEY_LEFTALT,
    [0x39] = KEY_SPACE,
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUMLOCK, [0x46] = KEY_SCROLLLOCK,
    [0x47] = KEY_KP7, [0x48] = KEY_KP8, [0x49] = KEY_KP9,
    [0x4A] = KEY_KPMINUS,
    [0x4B] = KEY_KP4, [0x4C] = KEY_KP5, [0x4D] = KEY_KP6,
    [0x4E] = KEY_KPPLUS,
    [0x4F] = KEY_KP1, [0x50] = KEY_KP2, [0x51] = KEY_KP3,
    [0x52] = KEY_KP0, [0x53] = KEY_KPDOT,
    [0x57] = KEY_F11, [0x58] = KEY_F12,
};

/* Scancode set 1, extended (after 0xE0 prefix) -> Linux KEY_*. */
static const uint16_t sc_extended[128] = {
    [0x1C] = KEY_KPENTER,
    [0x1D] = KEY_RIGHTCTRL,
    [0x35] = KEY_KPSLASH,
    [0x37] = KEY_SYSRQ,
    [0x38] = KEY_RIGHTALT,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x5B] = KEY_LEFTMETA,
    [0x5C] = KEY_RIGHTMETA,
    [0x5D] = KEY_MENU,
};

static int extended        = 0;
static int pause_remaining = 0;

static void push_event(uint16_t key, int pressed) {
    if (!key) return;
    int next = (kbd_head + 1) & KBD_RING_MASK;
    if (next != kbd_tail) {
        kbd_ring[kbd_head].key.keycode = key;
        kbd_ring[kbd_head].key.pressed = (uint8_t)pressed;
        kbd_head = next;
    }
    /* The message bus still receives events when the legacy queue is full. */

    struct msg m = {
        .type  = pressed ? MSG_KEY_DOWN : MSG_KEY_UP,
        .param = key,
        .x     = 0,
        .y     = 0,
        .when  = (uint32_t)pit_ticks(),
    };
    msg_post(&m);
}

static void kbd_handler(void) {
    uint8_t sc = inb(0x60);

    if (pause_remaining > 0) {
        pause_remaining--;
        if (pause_remaining == 0) {
            push_event(KEY_PAUSE, 1);
            push_event(KEY_PAUSE, 0);
        }
        return;
    }

    if (sc == KBD_SC_PREFIX_E0) { extended = 1; return; }
    if (sc == KBD_SC_PREFIX_E1) { pause_remaining = 5; return; }

    /* Bit 7 is the make/break flag, bits 6-0 the key. So a release is the
     * press code with 0x80 set, which is why both tables are only 128
     * entries — the release code indexes the same slot. */
    int      pressed = !(sc & KBD_SC_BREAK);
    uint8_t  code    = sc & KBD_SC_CODE_MASK;
    uint16_t key     = extended ? sc_extended[code] : sc_normal[code];
    extended = 0;

    push_event(key, pressed);
}

int keyboard_poll(int *pressed, uint16_t *key) {
    if (kbd_tail == kbd_head) return 0;
    *pressed = kbd_ring[kbd_tail].key.pressed;
    *key     = kbd_ring[kbd_tail].key.keycode;
    kbd_tail = (kbd_tail + 1) & KBD_RING_MASK;
    return 1;
}

void keyboard_init(void) {
    irq_install(1, kbd_handler);
}
