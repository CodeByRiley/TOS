#include "input/mouse.h"
#include "interrupts/idt.h"
#include "devices/io.h"
#include "devices/pit.h"
#include "msg/msg.h"
#include "utilities/log.h"
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX      0x20

/* Controller commands */
#define CTRL_READ_CCB    0x20
#define CTRL_WRITE_CCB   0x60
#define CTRL_ENABLE_AUX  0xA8
#define CTRL_WRITE_AUX   0xD4

/* Mouse commands */
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE       0xF4
#define MOUSE_ACK          0xFA

#define MOUSE_RING_SIZE 64
#define MOUSE_RING_MASK (MOUSE_RING_SIZE - 1)
_Static_assert((MOUSE_RING_SIZE & MOUSE_RING_MASK) == 0, "ring size must be pow2");

static struct mouse_event mouse_ring[MOUSE_RING_SIZE];
static volatile int       mouse_head = 0;
static volatile int       mouse_tail = 0;

static volatile int32_t cur_x = 0;
static volatile int32_t cur_y = 0;
static volatile uint8_t cur_buttons = 0;

static int32_t bound_w = 0;          /* 0 => unbounded */
static int32_t bound_h = 0;

static uint8_t pkt[3];
static int     pkt_idx = 0;

/* Wait for the controller's input buffer to be empty (we can write).
 * 100000 was picked the way every magic timeout in every kernel has ever
 * been picked: it worked once, nobody dared touch it. Real fix is to drive
 * this off PIT ticks; "real fix" has a habit of staying in the todo list. */
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

static int ctrl_send(uint8_t cmd) {
    if (wait_in_empty() != 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

static int data_send(uint8_t b) {
    if (wait_in_empty() != 0) return -1;
    outb(PS2_DATA, b);
    return 0;
}

static int data_recv(uint8_t *out) {
    if (wait_out_full() != 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

/* Send one byte to the mouse and read its ACK (0xFA). */
static int mouse_cmd(uint8_t cmd) {
    if (ctrl_send(CTRL_WRITE_AUX) != 0) return -1;
    if (data_send(cmd) != 0) return -1;
    uint8_t resp;
    if (data_recv(&resp) != 0) return -1;
    if (resp != MOUSE_ACK) return -1;
    return 0;
}

static void push_event(int16_t dx, int16_t dy, uint8_t buttons) {
    int next = (mouse_head + 1) & MOUSE_RING_MASK;
    if (next == mouse_tail) return;        /* drop on full */
    mouse_ring[mouse_head].dx      = dx;
    mouse_ring[mouse_head].dy      = dy;
    mouse_ring[mouse_head].buttons = buttons;
    mouse_ring[mouse_head]._pad    = 0;
    mouse_head = next;
}

static void parse_packet(void) {
    uint8_t b0 = pkt[0];
    /* Bit 3 of byte 0 is always set on a valid first packet byte. If it's
     * cleared we lost sync — drop and let the state machine recover. */
    if (!(b0 & 0x08)) {
        pkt_idx = 0;
        return;
    }

    /* Discard packets the mouse flagged as overflowing. */
    if (b0 & 0xC0) {
        pkt_idx = 0;
        return;
    }

    int16_t dx = (int16_t)pkt[1];
    int16_t dy = (int16_t)pkt[2];
    if (b0 & 0x10) dx |= (int16_t)0xFF00;          /* sign-extend X */
    if (b0 & 0x20) dy |= (int16_t)0xFF00;          /* sign-extend Y */
    /* PS/2 reports +Y as "up", we want screen coords (+Y down). */
    dy = -dy;

    uint8_t buttons = 0;
    if (b0 & 0x01) buttons |= MOUSE_BTN_LEFT;
    if (b0 & 0x02) buttons |= MOUSE_BTN_RIGHT;
    if (b0 & 0x04) buttons |= MOUSE_BTN_MIDDLE;

    uint8_t old_buttons = cur_buttons;
    cur_buttons = buttons;
    int32_t nx = cur_x + dx;
    int32_t ny = cur_y + dy;
    if (bound_w > 0) {
        if (nx < 0)              nx = 0;
        if (nx > bound_w - 1)    nx = bound_w - 1;
    }
    if (bound_h > 0) {
        if (ny < 0)              ny = 0;
        if (ny > bound_h - 1)    ny = bound_h - 1;
    }
    cur_x = nx;
    cur_y = ny;

    push_event(dx, dy, buttons);

    uint32_t when = (uint32_t)pit_ticks();
    int16_t  ax   = (int16_t)cur_x;
    int16_t  ay   = (int16_t)cur_y;

    if (dx || dy) {
        struct msg m = { .type = MSG_MOUSE_MOVE, .param = buttons,
                         .x = ax, .y = ay, .when = when };
        msg_post(&m);
    }

    uint8_t pressed_now  = buttons & ~old_buttons;
    uint8_t released_now = old_buttons & ~buttons;
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

static void mouse_handler(void) {
    /* Sanity: the ISR fires for IRQ12, but on some controllers spurious
     * shared writes can show up. Confirm aux bit before consuming the byte. */
    uint8_t st = inb(PS2_STATUS);
    if (!(st & PS2_STATUS_OUT_FULL)) return;
    if (!(st & PS2_STATUS_AUX))      { (void)inb(PS2_DATA); return; }

    uint8_t b = inb(PS2_DATA);
    pkt[pkt_idx++] = b;

    /* On byte 0, bit 3 must be set; resync if not. */
    if (pkt_idx == 1 && !(b & 0x08)) {
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
uint8_t mouse_buttons(void) { return cur_buttons; }

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
    uint8_t ccb;
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

    if (mouse_cmd(MOUSE_SET_DEFAULTS) != 0) {
        log_write("mouse: set-defaults rejected", KERNEL, LOG_ERROR);
        return;
    }
    if (mouse_cmd(MOUSE_ENABLE) != 0) {
        log_write("mouse: enable rejected", KERNEL, LOG_ERROR);
        return;
    }

    irq_install(12, mouse_handler);
    log_write("mouse: ready", KERNEL, LOG_INFO);
}
