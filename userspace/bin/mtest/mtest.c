#include "../../lib/syscall.h"
#include "../../include/key_codes.h"

extern int printf(const char *, ...);

#define CURSOR_SIZE 12
#define CURSOR_COLOR 0x00FFFFFFu       /* white */
#define BG_COLOR     0x00202040u       /* dark blue */
#define HIT_COLOR    0x00FF4040u       /* red on click */

/* Trail dot left at every MOUSE_DOWN site so we can see button events. */
#define MAX_DOTS 256
static struct { int16_t x, y; uint32_t color; } dots[MAX_DOTS];
static int dot_count = 0;

static void fill_rect(unsigned int *fb, struct fb_info *info,
                      int x, int y, int w, int h, unsigned int color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)info->width)  w = (int)info->width  - x;
    if (y + h > (int)info->height) h = (int)info->height - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = 0; yy < h; yy++) {
        unsigned int *row = (unsigned int *)((char *)fb + (y + yy) * info->pitch);
        for (int xx = 0; xx < w; xx++) {
            row[x + xx] = color;
        }
    }
}

static void clear_screen(unsigned int *fb, struct fb_info *info,
                         unsigned int color) {
    for (unsigned int y = 0; y < info->height; y++) {
        unsigned int *row = (unsigned int *)((char *)fb + y * info->pitch);
        for (unsigned int x = 0; x < info->width; x++) row[x] = color;
    }
}

static void redraw(unsigned int *fb, struct fb_info *info,
                   int cur_x, int cur_y, uint8_t buttons) {
    clear_screen(fb, info, BG_COLOR);

    /* Click trail. */
    for (int i = 0; i < dot_count; i++) {
        fill_rect(fb, info, dots[i].x - 2, dots[i].y - 2, 5, 5, dots[i].color);
    }

    /* Cursor. Color shifts when any button held so the user can see hits. */
    unsigned int color = buttons ? HIT_COLOR : CURSOR_COLOR;
    fill_rect(fb, info,
              cur_x - CURSOR_SIZE / 2, cur_y - CURSOR_SIZE / 2,
              CURSOR_SIZE, CURSOR_SIZE, color);
}

int main(void) {
    struct fb_info info;
    fb_info(&info);
    unsigned int *fb = (unsigned int *)fb_map();
    if (!fb) {
        printf("mtest: fb_map failed\n");
        return 1;
    }
    printf("mtest: fb %dx%d, ESC to quit\n", (int)info.width, (int)info.height);

    int cur_x = (int)info.width  / 2;
    int cur_y = (int)info.height / 2;
    uint8_t buttons = 0;

    /* Sync cursor to whatever the kernel already tracked. */
    {
        int32_t mx, my;
        uint8_t mb;
        mouse_pos(&mx, &my, &mb);
        cur_x = (int)mx;
        cur_y = (int)my;
        buttons = mb;
    }

    redraw(fb, &info, cur_x, cur_y, buttons);

    while (1) {
        struct msg m;
        int got_any = 0;
        while (msg_get(&m)) {
            got_any = 1;
            switch (m.type) {
            case MSG_KEY_DOWN:
                if (m.param == KEY_ESC) {
                    printf("mtest: ESC, exiting\n");
                    return 0;
                }
                printf("key down %d\n", (int)m.param);
                break;
            case MSG_KEY_UP:
                printf("key up   %d\n", (int)m.param);
                break;
            case MSG_MOUSE_MOVE:
                cur_x = m.x;
                cur_y = m.y;
                break;
            case MSG_MOUSE_DOWN:
                cur_x   = m.x;
                cur_y   = m.y;
                buttons = (uint8_t)m.param;
                if (dot_count < MAX_DOTS) {
                    unsigned int c = 0x00808080u;
                    if (m.param & MOUSE_BTN_LEFT)   c = 0x000080FFu;
                    if (m.param & MOUSE_BTN_RIGHT)  c = 0x00FFB000u;
                    if (m.param & MOUSE_BTN_MIDDLE) c = 0x0080FF80u;
                    dots[dot_count].x     = m.x;
                    dots[dot_count].y     = m.y;
                    dots[dot_count].color = c;
                    dot_count++;
                }
                printf("mouse down @%d,%d btns=%x\n",
                       (int)m.x, (int)m.y, (int)m.param);
                break;
            case MSG_MOUSE_UP:
                cur_x   = m.x;
                cur_y   = m.y;
                /* Strip released buttons from the held set we track locally. */
                buttons &= (uint8_t)~m.param;
                printf("mouse up   @%d,%d btns=%x\n",
                       (int)m.x, (int)m.y, (int)m.param);
                break;
            }
        }

        if (got_any) {
            redraw(fb, &info, cur_x, cur_y, buttons);
        }

        /* Cooperate so other tasks can run, then halt-loop until next event. */
        yield();
    }
    return 0;
}
