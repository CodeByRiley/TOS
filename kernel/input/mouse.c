/* kernel/input/mouse.c , mouse input driver.
 *
 * IRQ12 handler reads 3-byte (or 4-byte if intelliMouse) packets from
 * the AUX channel, converts them into relative motion + button mask,
 * updates the driver-tracked absolute cursor (clamped by
 * mouse_set_bounds), and posts MSG_MOUSE_* events to the input owner. USB HID
 * boot mice enter through mouse_hid_report() and use the same event path.
 *
 * Packet framing recovers from desync by looking for the "always 1" bit
 * in the first byte; we drop bytes until alignment looks plausible.
 */
#include <input/mouse.h>
#include <interrupts/idt.h>
#include <devices/io.h>
#include <devices/pit.h>
#include <msg/msg.h>
#include <utilities/log.h>
#include <stdint.h>

/* Port 0x60 is data for both devices; 0x64 is status on read, command on
 * write. Which device a byte came from is only knowable from the status
 * register's AUX bit, latched at the same time as the data. */
#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* PS/2 Status Register (read 0x64) */
//
// Bits | Name          | Description
// 7    | Parity Error  | 1 = Parity error on the last byte
// 6    | Timeout Error | 1 = Device did not respond in time
// 5    | AUX Data      | 1 = Byte in the output buffer came from the mouse
// 4    | Inhibit       | 0 = Keyboard locked
// 3    | Command/Data  | 0 = Last write went to 0x60, 1 = to 0x64
// 2    | System Flag   | Set after a successful POST
// 1    | Input Full    | 1 = Controller has not consumed our last write yet
// 0    | Output Full   | 1 = A byte is waiting to be read from 0x60
//
// "Input" and "Output" are from the controller's perspective: wait for
// Input Full to clear before writing, and for Output Full to set before
// reading.
#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX      0x20

/* Controller commands (write 0x64) */
//
// 0x20 | Read Controller Configuration Byte  | Result appears on 0x60
// 0x60 | Write Controller Configuration Byte | Next 0x60 write is the value
// 0xA8 | Enable AUX port                     | Mouse port is disabled at boot
// 0xD4 | Write to AUX device                 | Next 0x60 write goes to the mouse
//
// Without the 0xD4 prefix a byte written to 0x60 goes to the keyboard, so
// every mouse command needs it , one prefix per byte, not per sequence.
#define CTRL_READ_CCB    0x20
#define CTRL_WRITE_CCB   0x60
#define CTRL_ENABLE_AUX  0xA8
#define CTRL_WRITE_AUX   0xD4

/* Controller Configuration Byte (read/written by 0x20 / 0x60) */
//
// Bits | Name             | Description
// 7    | Reserved         |
// 6    | Translation      | 1 = Translate keyboard to scancode set 1
// 5    | AUX Clock Off    | 1 = Mouse clock disabled
// 4    | KBD Clock Off    | 1 = Keyboard clock disabled
// 3    | Reserved         |
// 2    | System Flag      | Mirrors status bit 2
// 1    | AUX Interrupt    | 1 = Mouse raises IRQ12
// 0    | KBD Interrupt    | 1 = Keyboard raises IRQ1
#define CCB_AUX_IRQ      0x02
#define CCB_AUX_CLOCK_ON 0x20

/* Mouse commands (sent through CTRL_WRITE_AUX). Each is answered with
 * MOUSE_ACK, which must be consumed before sending the next one. */
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE       0xF4
#define MOUSE_ACK          0xFA
#define MOUSE_SMPL_RATE    0xF3 // Set Sample Rate
#define MOUSE_SET_RES	     0xE8

#define RES_1_COUNT 0x00
#define RES_2_COUNT 0x01
#define RES_4_COUNT 0x02
#define RES_8_COUNT 0x03

#define SAMPLE_10HZ 10
#define SAMPLE_20HZ 20
#define SAMPLE_40HZ 40
#define SAMPLE_60HZ 60
#define SAMPLE_80HZ 80
#define SAMPLE_100HZ 100
#define SAMPLE_200HZ 200


/* Mouse packet, byte 0 */
//
// Bits | Name          | Description
// 7    | Y Overflow    | 1 = Y movement exceeded the 9-bit range
// 6    | X Overflow    | 1 = X movement exceeded the 9-bit range
// 5    | Y Sign        | Sign bit for byte 2
// 4    | X Sign        | Sign bit for byte 1
// 3    | Always One    | Reads 1 on a valid first byte , the only framing marker
// 2    | Middle Button |
// 1    | Right Button  |
// 0    | Left Button   |
//
// Bit 3 is the whole resync story: the stream carries no other framing, so a
// dropped byte is only detectable by byte 0 arriving without it set.
#define MOUSE_PKT_ALWAYS_ONE 0x08
#define MOUSE_PKT_X_SIGN     0x10
#define MOUSE_PKT_Y_SIGN     0x20
#define MOUSE_PKT_X_OVERFLOW 0x40
#define MOUSE_PKT_Y_OVERFLOW 0x80

#define MOUSE_RING_SIZE 64
#define MOUSE_RING_MASK (MOUSE_RING_SIZE - 1)
_Static_assert((MOUSE_RING_SIZE & MOUSE_RING_MASK) == 0, "ring size must be pow2");

static struct mouse_event mouse_ring[MOUSE_RING_SIZE];
static volatile int       mouse_head = 0;
static volatile int       mouse_tail = 0;

static volatile int32_t cur_x = 0;
static volatile int32_t cur_y = 0;
static volatile u8 cur_buttons = 0;

static int32_t bound_w = 0;          /* 0 => unbounded */
static int32_t bound_h = 0;

static u8 pkt[3];
static int     pkt_idx = 0;

/* The bounded poll prevents a stuck controller from hanging boot.
 * TODO: Use a PIT-based deadline. */
static int wait_in_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_IN_FULL)) return 0;
    }
    return -1;
}

/* Wait for the controller's output buffer to fill (data ready to read). */
static int wait_out_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT_FULL) return 0;
    }
    return -1;
}

static int ctrl_send(u8 cmd) {
    if (wait_in_empty() != 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

static int data_send(u8 b) {
    if (wait_in_empty() != 0) return -1;
    outb(PS2_DATA, b);
    return 0;
}

static int data_recv(u8 *out) {
    if (wait_out_full() != 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

/* Send one byte to the mouse and read its ACK (0xFA). */
static int mouse_cmd(u8 cmd) {
    if (ctrl_send(CTRL_WRITE_AUX) != 0) return -1;
    if (data_send(cmd) != 0) return -1;
    u8 resp;
    if (data_recv(&resp) != 0) return -1;
    if (resp != MOUSE_ACK) return -1;
    return 0;
}

/* Send a command that requires an argument byte (e.g., sample rate, resolution) */
static int mouse_cmd_arg(u8 cmd, u8 arg) {
    if (mouse_cmd(cmd) != 0) return -1;       /* Send command, expect ACK */
    if (ctrl_send(CTRL_WRITE_AUX) != 0) return -1;
    if (data_send(arg) != 0) return -1;       /* Send argument */

    u8 resp;
    if (data_recv(&resp) != 0) return -1;     /* Expect second ACK */
    if (resp != MOUSE_ACK) return -1;
    return 0;
}

static void push_event(int16_t dx, int16_t dy, u8 buttons) {
    int next = (mouse_head + 1) & MOUSE_RING_MASK;
    if (next == mouse_tail) return;        /* drop on full */
    mouse_ring[mouse_head].dx      = dx;
    mouse_ring[mouse_head].dy      = dy;
    mouse_ring[mouse_head].buttons = buttons;
    mouse_ring[mouse_head]._pad    = 0;
    mouse_head = next;
}

static int32_t clamp_axis(int32_t v, int32_t bound) {
    if (bound <= 0) return v;
    if (v < 0) return 0;
    if (v > bound - 1) return bound - 1;
    return v;
}

static int16_t delta_i16(int32_t d) {
    if (d < -32768) return -32768;
    if (d > 32767)  return 32767;
    return (int16_t)d;
}

static void submit_at(int16_t dx, int16_t dy, int32_t nx, int32_t ny,
                      u8 buttons) {
    u8 old_buttons = cur_buttons;
    cur_buttons = buttons;
    cur_x = nx;
    cur_y = ny;

    push_event(dx, dy, buttons);

    u32 when = (u32)pit_ticks();
    int16_t  ax   = (int16_t)cur_x;
    int16_t  ay   = (int16_t)cur_y;

    if (dx || dy) {
        struct msg m = { .type = MSG_MOUSE_MOVE, .param = buttons,
                         .x = ax, .y = ay, .when = when };
        msg_post(&m);
    }

    u8 pressed_now  = buttons & ~old_buttons;
    u8 released_now = old_buttons & ~buttons;
    if (pressed_now) {
        struct msg m = { .type = MSG_MOUSE_DOWN, .param = pressed_now,
                         .x = ax, .y = ay, .when = when };
        msg_post(&m);
    }
    if (released_now) {
        struct msg m = { .type = MSG_MOUSE_UP, .param = released_now,
                         .x = ax, .y = ay, .when = when };
        msg_post(&m);
    }
}

static void submit_relative(int16_t dx, int16_t dy, u8 buttons) {
    int32_t nx = clamp_axis(cur_x + dx, bound_w);
    int32_t ny = clamp_axis(cur_y + dy, bound_h);
    submit_at(dx, dy, nx, ny, buttons);
}

static void submit_absolute(int32_t nx, int32_t ny, u8 buttons) {
    nx = clamp_axis(nx, bound_w);
    ny = clamp_axis(ny, bound_h);
    submit_at(delta_i16(nx - cur_x), delta_i16(ny - cur_y), nx, ny, buttons);
}

static void parse_packet(void) {
    u8 b0 = pkt[0];
    /* Lost sync , drop and let the state machine recover. */
    if (!(b0 & MOUSE_PKT_ALWAYS_ONE)) {
        pkt_idx = 0;
        return;
    }

    /* Discard packets the mouse flagged as overflowing. */
    if (b0 & (MOUSE_PKT_X_OVERFLOW | MOUSE_PKT_Y_OVERFLOW)) {
        pkt_idx = 0;
        return;
    }

    /* Movement is 9-bit two's complement: 8 bits of magnitude in bytes 1-2
     * and the sign carried back in byte 0. */
    int16_t dx = (int16_t)pkt[1];
    int16_t dy = (int16_t)pkt[2];
    if (b0 & MOUSE_PKT_X_SIGN) dx |= (int16_t)0xFF00;
    if (b0 & MOUSE_PKT_Y_SIGN) dy |= (int16_t)0xFF00;
    /* PS/2 reports +Y as "up", we want screen coords (+Y down). */
    dy = -dy;

    u8 buttons = 0;
    if (b0 & 0x01) buttons |= MOUSE_BTN_LEFT;
    if (b0 & 0x02) buttons |= MOUSE_BTN_RIGHT;
    if (b0 & 0x04) buttons |= MOUSE_BTN_MIDDLE;

    submit_relative(dx, dy, buttons);
}

void mouse_hid_report(const u8 *report, u16 len) {
    if (!report || len < 3) return;

    u8 buttons = 0;
    if (report[0] & 0x01) buttons |= MOUSE_BTN_LEFT;
    if (report[0] & 0x02) buttons |= MOUSE_BTN_RIGHT;
    if (report[0] & 0x04) buttons |= MOUSE_BTN_MIDDLE;

    /* HID boot mice report signed relative X/Y in screen orientation:
     * +X is right, +Y is down. Wheel and extra buttons are ignored here. */
    submit_relative((int16_t)(int8_t)report[1],
                    (int16_t)(int8_t)report[2], buttons);
}

void mouse_hid_tablet_report(const u8 *report, u16 len) {
    if (!report || len < 5) return;

    u8 buttons = 0;
    if (report[0] & 0x01) buttons |= MOUSE_BTN_LEFT;
    if (report[0] & 0x02) buttons |= MOUSE_BTN_RIGHT;
    if (report[0] & 0x04) buttons |= MOUSE_BTN_MIDDLE;

    u32 raw_x = (u32)report[1] | ((u32)report[2] << 8);
    u32 raw_y = (u32)report[3] | ((u32)report[4] << 8);
    int32_t nx = bound_w > 1 ? (int32_t)((raw_x * (u32)(bound_w - 1)) /
                                         0x7FFFu)
                             : (int32_t)raw_x;
    int32_t ny = bound_h > 1 ? (int32_t)((raw_y * (u32)(bound_h - 1)) /
                                         0x7FFFu)
                             : (int32_t)raw_y;
    submit_absolute(nx, ny, buttons);
}

static void mouse_handler(void) {
    /* Sanity: the ISR fires for IRQ12, but on some controllers spurious
     * shared writes can show up. Confirm aux bit before consuming the byte. */
    u8 st = inb(PS2_STATUS);
    if (!(st & PS2_STATUS_OUT_FULL)) return;
    if (!(st & PS2_STATUS_AUX))      { (void)inb(PS2_DATA); return; }

    u8 b = inb(PS2_DATA);
    pkt[pkt_idx++] = b;

    if (pkt_idx == 1 && !(b & MOUSE_PKT_ALWAYS_ONE)) {
        pkt_idx = 0;
        return;
    }
    if (pkt_idx == 3) {
        parse_packet();
        pkt_idx = 0;
    }
}

int mouse_poll(struct mouse_event *out) {
    if (mouse_tail == mouse_head) return 0;
    *out = mouse_ring[mouse_tail];
    mouse_tail = (mouse_tail + 1) & MOUSE_RING_MASK;
    return 1;
}

int32_t mouse_x(void)       { return cur_x; }
int32_t mouse_y(void)       { return cur_y; }
u8 mouse_buttons(void) { return cur_buttons; }

void mouse_set_bounds(int32_t w, int32_t h) {
    bound_w = w;
    bound_h = h;
    if (bound_w > 0 && cur_x > bound_w - 1) cur_x = bound_w - 1;
    if (bound_h > 0 && cur_y > bound_h - 1) cur_y = bound_h - 1;
}

void mouse_init(void) {
    /* Drain anything stale from the controller output buffer. */
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUT_FULL)) break;
        (void)inb(PS2_DATA);
    }

    if (ctrl_send(CTRL_ENABLE_AUX) != 0) {
        log_write("mouse: enable aux failed", KERNEL, LOG_ERROR);
        return;
    }

    /* Read CCB, set IRQ12 enable + clear aux clock disable, write back. */
    if (ctrl_send(CTRL_READ_CCB) != 0) {
        log_write("mouse: read CCB failed", KERNEL, LOG_ERROR);
        return;
    }
    u8 ccb;
    if (data_recv(&ccb) != 0) {
        log_write("mouse: read CCB data failed", KERNEL, LOG_ERROR);
        return;
    }
    ccb |=  0x02;            /* enable IRQ12 */
    ccb &= ~0x20;            /* enable aux clock */
    if (ctrl_send(CTRL_WRITE_CCB) != 0 || data_send(ccb) != 0) {
        log_write("mouse: write CCB failed", KERNEL, LOG_ERROR);
        return;
    }

    /* Reset to defaults (stops any prior streaming) */
    if (mouse_cmd(MOUSE_SET_DEFAULTS) != 0) {
        log_write("mouse: set-defaults rejected", KERNEL, LOG_ERROR);
        return;
    }

    /* Set sample rate */
    if (mouse_cmd_arg(MOUSE_SMPL_RATE, SAMPLE_200HZ) != 0) {
        log_write("mouse: set sample rate failed", KERNEL, LOG_ERROR);
        return;
    }

    /* Set resolution to 8 counts/mm (0x03) */
    if (mouse_cmd_arg(MOUSE_SET_RES, RES_8_COUNT) != 0) {
        log_write("mouse: set resolution failed", KERNEL, LOG_ERROR);
        return;
    }

    /* Enable streaming last, so config commands aren't mixed with data */
    if (mouse_cmd(MOUSE_ENABLE) != 0) {
        log_write("mouse: enable rejected", KERNEL, LOG_ERROR);
        return;
    }

    irq_install(12, mouse_handler);
    log_write("mouse: ready", KERNEL, LOG_INFO);
}
