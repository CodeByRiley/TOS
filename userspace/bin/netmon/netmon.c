/* userspace/bin/netmon/netmon.c — live view of the NIC.
 *
 * TOS has a driver and no stack: the e1000 answers ARP and ICMP echo and
 * nothing above that exists yet. That makes `ping` from the host the only
 * end-to-end test there is, and reading the result off the serial log
 * means picking the interesting bytes out of unrelated kernel output.
 * This shows them instead — counters, a decoded frame list, and a hex
 * dump of whichever frame is selected.
 *
 * Everything here is read-only. The two syscalls behind it (SYS_NET_STATS
 * and SYS_NET_CAPTURE) expose the capture ring in kernel/net/netmon.c and
 * offer no way to send, because there is nothing yet to send through.
 *
 * The kernel ring is small and is meant to be drained continuously. This
 * keeps a much longer local history so scrollback survives a pause, and
 * notices frames that were overwritten before it got to them by comparing
 * each batch's first sequence number against the cursor it asked with.
 */
#include <lib/syscall.h>
#include <lib/wm.h>
#include <lib/gfx.h>
#include <lib/ui.h>
#include <include/key_codes.h>

#include <stdio.h>
#include <string.h>

#define WIN_W 720
#define WIN_H 540

#define MARGIN     8
#define ROW_H     10          /* 8px glyph cell plus leading */
#define HEX_COLS  16
#define HEX_ROWS  (NET_FRAME_BYTES / HEX_COLS)

/* Frames kept on this side. The kernel ring is 64 and wraps quickly under
 * a ping flood; this is the scrollback. */
#define HISTORY 512

#define COL_BG      0x00101418u
#define COL_TEXT    0x00D8E0E8u
#define COL_DIM     0x00808C98u
#define COL_RX      0x0070C0FFu
#define COL_TX      0x00FFC060u
#define COL_SEL     0x00204058u
#define COL_OK      0x0060D080u
#define COL_BAD     0x00E06060u

/* Ethernet and IPv4 constants, spelled out because userspace has no
 * netinet/ to take them from yet. */
#define ETH_HDR_LEN   14
#define ETH_TYPE_IPV4 0x0800u
#define ETH_TYPE_ARP  0x0806u
#define ETH_TYPE_IPV6 0x86DDu
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u

struct app {
    struct net_frame history[HISTORY];
    int      count;           /* frames held, capped at HISTORY      */
    int      first;           /* ring index of the oldest frame      */
    uint64_t cursor;          /* next kernel sequence wanted         */
    uint64_t missed;          /* frames the ring dropped before us   */

    int selected;             /* index in logical order, -1 for none */
    int scroll;               /* first visible row                   */
    int follow;               /* stick to the newest frame           */
    int paused;
};

/* ---------------- formatting helpers ----------------------------------- */

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void fmt_ip(char *out, size_t cap, const uint8_t *ip) {
    snprintf(out, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void fmt_mac(char *out, size_t cap, const uint8_t *mac) {
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* One-line description of a frame, in the spirit of tcpdump's default
 * output. Every branch re-checks the captured length because a frame is
 * truncated at NET_FRAME_BYTES and a short capture must not be read past.
 */
static void summarize(const struct net_frame *f, char *out, size_t cap) {
    const uint8_t *p = f->data;
    uint32_t n = f->captured;

    if (n < ETH_HDR_LEN) {
        snprintf(out, cap, "runt (%u bytes captured)", n);
        return;
    }

    uint16_t type = be16(p + 12);
    const uint8_t *body = p + ETH_HDR_LEN;
    uint32_t body_len = n - ETH_HDR_LEN;

    if (type == ETH_TYPE_ARP) {
        if (body_len < 28) {
            snprintf(out, cap, "ARP  (truncated)");
            return;
        }
        uint16_t op = be16(body + 6);
        char spa[16], tpa[16], sha[18];
        fmt_ip(spa, sizeof(spa), body + 14);
        fmt_ip(tpa, sizeof(tpa), body + 24);
        fmt_mac(sha, sizeof(sha), body + 8);
        if (op == 1)
            snprintf(out, cap, "ARP  who-has %s tell %s", tpa, spa);
        else if (op == 2)
            snprintf(out, cap, "ARP  %s is-at %s", spa, sha);
        else
            snprintf(out, cap, "ARP  opcode %u", op);
        return;
    }

    if (type == ETH_TYPE_IPV6) {
        snprintf(out, cap, "IPv6");
        return;
    }

    if (type != ETH_TYPE_IPV4) {
        snprintf(out, cap, "ethertype 0x%04x", type);
        return;
    }

    if (body_len < 20) {
        snprintf(out, cap, "IPv4 (truncated)");
        return;
    }

    uint32_t ihl = (uint32_t)(body[0] & 0x0Fu) * 4u;
    uint8_t proto = body[9];
    char src[16], dst[16];
    fmt_ip(src, sizeof(src), body + 12);
    fmt_ip(dst, sizeof(dst), body + 16);

    const uint8_t *l4 = body + ihl;
    uint32_t l4_len = body_len > ihl ? body_len - ihl : 0;

    if (proto == IP_PROTO_ICMP && l4_len >= 4) {
        const char *kind = 0;
        switch (l4[0]) {
        case 0:  kind = "echo-reply";    break;
        case 3:  kind = "unreachable";   break;
        case 8:  kind = "echo-request";  break;
        case 11: kind = "time-exceeded"; break;
        default: break;
        }
        if (kind)
            snprintf(out, cap, "ICMP %s  %s > %s", kind, src, dst);
        else
            snprintf(out, cap, "ICMP type %u  %s > %s", l4[0], src, dst);
        return;
    }

    if ((proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) && l4_len >= 4) {
        snprintf(out, cap, "%s  %s:%u > %s:%u",
                 proto == IP_PROTO_TCP ? "TCP" : "UDP",
                 src, be16(l4), dst, be16(l4 + 2));
        return;
    }

    snprintf(out, cap, "IPv4 proto %u  %s > %s", proto, src, dst);
}

/* ---------------- capture ---------------------------------------------- */

static struct net_frame *frame_at(struct app *a, int index) {
    return &a->history[(a->first + index) % HISTORY];
}

static void history_push(struct app *a, const struct net_frame *f) {
    if (a->count < HISTORY) {
        a->history[(a->first + a->count) % HISTORY] = *f;
        a->count++;
        return;
    }

    a->history[a->first] = *f;
    a->first = (a->first + 1) % HISTORY;
    /* The oldest frame just went away, so every index shifted down one.
     * Move the selection and the viewport with it, or both would slide
     * onto a neighbouring frame with no input from the user. */
    if (a->selected > 0)
        a->selected--;
    if (a->scroll > 0)
        a->scroll--;
}

static void drain_capture(struct app *a) {
    struct net_frame batch[NET_CAPTURE_BATCH];

    /* Bounded so a busy link cannot starve the redraw; whatever is left
     * gets picked up next frame. */
    for (int round = 0; round < 8; round++) {
        uint64_t asked = a->cursor;
        long n = net_capture(&a->cursor, batch, NET_CAPTURE_BATCH);
        if (n <= 0)
            break;
        if (batch[0].seq > asked)
            a->missed += batch[0].seq - asked;
        for (long i = 0; i < n; i++)
            history_push(a, &batch[i]);
        if (n < NET_CAPTURE_BATCH)
            break;
    }
}

/* ---------------- drawing ---------------------------------------------- */

static void draw_header(struct gfx_surface *s, struct ui_context *ui,
                        struct gfx_rect r, const struct net_stats *st,
                        const struct app *a) {
    ui_panel(ui, r);
    struct gfx_rect in = gfx_rect_inset(r, 6);
    char line[128];

    if (!st->present) {
        gfx_text(s, in.x, in.y, "no NIC bound", COL_BAD, 1);
        gfx_text(s, in.x, in.y + ROW_H * 2,
                 "e1000 never probed - check the PCI scan on the serial log",
                 COL_DIM, 1);
        return;
    }

    char mac[18], ip[16];
    fmt_mac(mac, sizeof(mac), st->mac);
    fmt_ip(ip, sizeof(ip), st->ipv4);

    gfx_text(s, in.x, in.y, "e1000", COL_TEXT, 1);
    gfx_text(s, in.x + 56, in.y, st->link_up ? "link up" : "link down",
             st->link_up ? COL_OK : COL_BAD, 1);
    snprintf(line, sizeof(line), "%u Mb/s", st->speed_mbps);
    gfx_text(s, in.x + 144, in.y, line, COL_DIM, 1);
    snprintf(line, sizeof(line), "MAC %s    IP %s", mac, ip);
    gfx_text(s, in.x + 232, in.y, line, COL_DIM, 1);

    snprintf(line, sizeof(line), "RX %llu frames  %llu bytes",
             (unsigned long long)st->rx_frames,
             (unsigned long long)st->rx_bytes);
    gfx_text(s, in.x, in.y + ROW_H * 2, line, COL_RX, 1);

    snprintf(line, sizeof(line), "TX %llu frames  %llu bytes",
             (unsigned long long)st->tx_frames,
             (unsigned long long)st->tx_bytes);
    gfx_text(s, in.x + 288, in.y + ROW_H * 2, line, COL_TX, 1);

    snprintf(line, sizeof(line), "held %d%s", a->count,
             a->paused ? "   PAUSED" : "");
    gfx_text(s, in.x + 576, in.y + ROW_H * 2, line,
             a->paused ? COL_TX : COL_DIM, 1);

    if (a->missed) {
        snprintf(line, sizeof(line),
                 "%llu frames lost to ring overwrite",
                 (unsigned long long)a->missed);
        gfx_text(s, in.x, in.y + ROW_H * 3, line, COL_BAD, 1);
    }
}

static void draw_list(struct gfx_surface *s, struct ui_context *ui,
                      struct gfx_rect r, struct app *a) {
    ui_well(ui, r);
    struct gfx_rect in = gfx_rect_inset(r, 4);
    gfx_fill(s, in, COL_BG);

    struct gfx_rect prev = gfx_clip_push(s, in);
    int rows = in.h / ROW_H;

    gfx_text(s, in.x + 4, in.y, "SEQ      DIR  LEN    SUMMARY", COL_DIM, 1);

    for (int i = 0; i < rows - 1; i++) {
        int index = a->scroll + i;
        if (index >= a->count)
            break;

        const struct net_frame *f = frame_at(a, index);
        int y = in.y + (i + 1) * ROW_H;

        if (index == a->selected)
            gfx_fill(s, gfx_rect_make(in.x, y - 1, in.w, ROW_H), COL_SEL);

        char line[168], summary[120];
        summarize(f, summary, sizeof(summary));
        snprintf(line, sizeof(line), "%-8llu %s   %-6u %s",
                 (unsigned long long)f->seq,
                 f->direction == NET_DIR_TX ? "TX" : "RX",
                 f->length, summary);
        gfx_text(s, in.x + 4, y, line,
                 f->direction == NET_DIR_TX ? COL_TX : COL_RX, 1);
    }

    if (a->count == 0)
        gfx_text(s, in.x + 4, in.y + ROW_H * 2,
                 "nothing captured yet - try: ping 10.0.2.30", COL_DIM, 1);

    gfx_clip_set(s, prev);
}

static void draw_hex(struct gfx_surface *s, struct ui_context *ui,
                     struct gfx_rect r, struct app *a) {
    ui_well(ui, r);
    struct gfx_rect in = gfx_rect_inset(r, 4);
    gfx_fill(s, in, COL_BG);

    if (a->selected < 0 || a->selected >= a->count) {
        gfx_text(s, in.x + 4, in.y, "no frame selected", COL_DIM, 1);
        return;
    }

    const struct net_frame *f = frame_at(a, a->selected);
    struct gfx_rect prev = gfx_clip_push(s, in);

    for (int row = 0; row < HEX_ROWS; row++) {
        uint32_t base = (uint32_t)row * HEX_COLS;
        if (base >= f->captured)
            break;

        char line[96];
        int at = snprintf(line, sizeof(line), "%04x  ", base);

        for (int col = 0; col < HEX_COLS; col++) {
            uint32_t off = base + (uint32_t)col;
            size_t room = sizeof(line) - (size_t)at;
            if (off < f->captured)
                at += snprintf(line + at, room, "%02x ", f->data[off]);
            else
                at += snprintf(line + at, room, "   ");
            if (col == 7)
                at += snprintf(line + at, sizeof(line) - (size_t)at, " ");
        }

        at += snprintf(line + at, sizeof(line) - (size_t)at, " |");
        for (int col = 0; col < HEX_COLS; col++) {
            uint32_t off = base + (uint32_t)col;
            char c = ' ';
            if (off < f->captured) {
                uint8_t b = f->data[off];
                c = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            if ((size_t)at + 2 < sizeof(line))
                line[at++] = c;
        }
        if ((size_t)at + 1 < sizeof(line))
            line[at++] = '|';
        line[at] = 0;

        gfx_text(s, in.x + 4, in.y + row * ROW_H, line, COL_TEXT, 1);
    }

    if (f->captured < f->length) {
        char note[80];
        snprintf(note, sizeof(note), "%u of %u bytes captured",
                 f->captured, f->length);
        gfx_text(s, in.x + in.w - 200, in.y, note, COL_DIM, 1);
    }

    gfx_clip_set(s, prev);
}

/* ---------------- selection -------------------------------------------- */

static void clamp_view(struct app *a, int rows) {
    if (a->count == 0) {
        a->selected = -1;
        a->scroll = 0;
        return;
    }
    if (a->selected < 0)
        a->selected = a->count - 1;
    if (a->selected >= a->count)
        a->selected = a->count - 1;

    if (a->selected < a->scroll)
        a->scroll = a->selected;
    if (a->selected >= a->scroll + rows)
        a->scroll = a->selected - rows + 1;
    if (a->scroll < 0)
        a->scroll = 0;
}

static void select_delta(struct app *a, int delta) {
    if (a->count == 0)
        return;

    a->selected += delta;
    if (a->selected < 0)
        a->selected = 0;
    if (a->selected >= a->count)
        a->selected = a->count - 1;

    /* Following means "stay pinned to the newest frame"; stepping off the
     * last row is how the user says they would rather read history. */
    a->follow = (a->selected == a->count - 1);
}

int main(void) {
    /* static: struct app carries the 512-frame history, which is far too
     * large for the 8 KiB user stack crt1 hands out. */
    static struct app app;
    app.selected = -1;
    app.follow = 1;

    struct wm_window win;
    if (wm_window_create_ex(WIN_W, WIN_H, "netmon", WM_CREATE_STATUSBAR,
                            &win) != 0) {
        printf("netmon: wm_window_create failed\n");
        return 1;
    }

    struct gfx_surface surface;
    gfx_surface_init(&surface, (uint32_t *)(uintptr_t)win.surface_va,
                     win.w, win.h, (int)(win.pitch / 4));

    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));

    /* winman truncates the status strip at 47 bytes, so this is as much
     * as fits; the rest of the keys are in the header comment. */
    wm_window_set_status(win.handle,
                         "ESC quit  UP/DN PGUP/PGDN  P pause  C clear");

    int mouse_x = 0, mouse_y = 0, buttons = 0;
    int list_rows = 1;

    for (;;) {
        struct wm_event ev;
        while (wm_poll_event(&ev)) {
            switch (ev.type) {
            case WM_EV_KEY_DOWN:
                switch (ev.param) {
                case KEY_ESC:
                    wm_window_destroy(win.handle);
                    return 0;
                case KEY_UP:       select_delta(&app, -1);         break;
                case KEY_DOWN:     select_delta(&app, 1);          break;
                case KEY_PAGEUP:   select_delta(&app, -list_rows); break;
                case KEY_PAGEDOWN: select_delta(&app, list_rows);  break;
                case KEY_HOME:
                    app.selected = 0;
                    app.follow = 0;
                    break;
                case KEY_END:
                    app.selected = app.count - 1;
                    app.follow = 1;
                    break;
                case KEY_P:
                    app.paused = !app.paused;
                    break;
                case KEY_C:
                    app.count = 0;
                    app.first = 0;
                    app.selected = -1;
                    app.scroll = 0;
                    app.missed = 0;
                    app.follow = 1;
                    break;
                default:
                    break;
                }
                break;
            case WM_EV_MOUSE_MOVE:
            case WM_EV_MOUSE_DOWN:
            case WM_EV_MOUSE_UP:
                mouse_x = ev.x;
                mouse_y = ev.y;
                buttons = ev.param;
                break;
            case WM_EV_RESIZE:
                gfx_surface_init(&surface,
                                 (uint32_t *)(uintptr_t)ev.surface_va,
                                 ev.w, ev.h, (int)(ev.pitch / 4));
                win.surface_va = ev.surface_va;
                win.pitch = ev.pitch;
                win.w = ev.w;
                win.h = ev.h;
                break;
            case WM_EV_QUIT:
                wm_window_destroy(win.handle);
                return 0;
            default:
                break;
            }
        }

        /* Pausing freezes the display, not the capture: the cursor keeps
         * advancing so the kernel ring is still drained and a resumed
         * monitor is not staring at ancient frames. What pause drops is
         * the history, which is what it is being asked for. */
        if (app.paused) {
            struct net_frame discard[NET_CAPTURE_BATCH];
            net_capture(&app.cursor, discard, NET_CAPTURE_BATCH);
        } else {
            drain_capture(&app);
            if (app.follow)
                app.selected = app.count - 1;
        }

        struct net_stats st;
        memset(&st, 0, sizeof(st));
        net_stats(&st);

        int header_h = ROW_H * 4 + 12;
        int hex_h = HEX_ROWS * ROW_H + 12;

        struct gfx_rect header =
            gfx_rect_make(MARGIN, MARGIN, win.w - MARGIN * 2, header_h);
        int list_y = header.y + header.h + MARGIN;
        int hex_y = win.h - MARGIN - hex_h;
        struct gfx_rect list =
            gfx_rect_make(MARGIN, list_y, win.w - MARGIN * 2,
                          hex_y - MARGIN - list_y);
        struct gfx_rect hex =
            gfx_rect_make(MARGIN, hex_y, win.w - MARGIN * 2, hex_h);

        list_rows = (gfx_rect_inset(list, 4).h / ROW_H) - 1;
        if (list_rows < 1)
            list_rows = 1;
        clamp_view(&app, list_rows);

        ui_begin(&ui, &surface, 0, mouse_x, mouse_y, buttons);
        gfx_clear(&surface, ui_theme_default.face);
        draw_header(&surface, &ui, header, &st, &app);
        draw_list(&surface, &ui, list, &app);
        draw_hex(&surface, &ui, hex, &app);
        ui_end(&ui);

        wm_window_invalidate(win.handle);
        sleep_ticks(2);
    }
}
