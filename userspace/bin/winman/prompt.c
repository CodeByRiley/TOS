#define WINMAN_DECLARE_STATE
#include "winman.h"
#include "key_codes.h"
#include "syscall.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* ---------------- Modal prompt ------------------------------------------
 *
 * Winman owns the dialog outright: it draws it, it consumes every keystroke
 * and click while it is up, and it sends the answer back. The requesting app
 * is parked inside wm_prompt() and never sees the input, which is what makes
 * the dialog genuinely modal rather than merely painted on top.
 *
 * Only one is allowed at a time. A second request while one is open is
 * refused with WM_PROMPT_CANCEL rather than queued , an app blocked in
 * wm_prompt() cannot have issued it, so it came from a second app, and
 * stacking dialogs from unrelated apps has no sane z-order answer. */
struct prompt_state prompt;

/* Shift state for the dialog's text field. Tracked here rather than reusing
 * the client-facing modifier state, because keystrokes that reach a prompt
 * never reach a client and vice versa. */
int shift_held = 0;
/* Ctrl state, for the console zoom shortcuts. */
int ctrl_held = 0;

const char *prompt_button_label(int kind, int idx) {
  if (kind == WM_PROMPT_CONFIRM) {
    static const char *labels[] = {"Yes", "No", "Cancel"};
    return (idx >= 0 && idx < 3) ? labels[idx] : "";
  }
  if (kind == WM_PROMPT_TEXT) {
    static const char *labels[] = {"OK", "Cancel"};
    return (idx >= 0 && idx < 2) ? labels[idx] : "";
  }
  return idx == 0 ? "OK" : "";
}

int prompt_button_count(int kind) {
  if (kind == WM_PROMPT_CONFIRM)
    return 3;
  if (kind == WM_PROMPT_TEXT)
    return 2;
  return 1;
}

/* Result for a given button index, so the click path and the keyboard path
 * cannot disagree about what "the second button" means. */
int prompt_button_result(int kind, int idx) {
  if (kind == WM_PROMPT_CONFIRM)
    return idx == 0 ? WM_PROMPT_OK
                    : (idx == 1 ? WM_PROMPT_NO : WM_PROMPT_CANCEL);
  if (kind == WM_PROMPT_TEXT)
    return idx == 0 ? WM_PROMPT_OK : WM_PROMPT_CANCEL;
  return WM_PROMPT_OK;
}

/* Freeze dialog geometry so draw and damage paths use the same rect. */
void prompt_freeze_rect(void) {
  prompt.w = PROMPT_W;
  prompt.h = PROMPT_H;

  int wx = 0, wy = 0, wcw = 0, wch = 0;

  /* Prefer the owner window; otherwise use the screen center. */
  if (prompt.owner_handle > 0 &&
      win_get_rect(prompt.owner_handle, &wx, &wy, &wcw, &wch)) {
    int o_w = outer_w_dims(wcw);
    int o_h = outer_h_dims(wch, status_h_of(prompt.owner_handle));

    prompt.x = wx + (o_w - PROMPT_W) / 2;
    prompt.y = wy + (o_h - PROMPT_H) / 2;
  } else {
    prompt.x = (fb_w - PROMPT_W) / 2;
    prompt.y = (fb_h - TASKBAR_PX - PROMPT_H) / 2;
  }

  /* Keep the dialog on-screen. */
  if (prompt.x < 0)
    prompt.x = 0;
  if (prompt.y < 0)
    prompt.y = 0;
  if (prompt.x + prompt.w > fb_w)
    prompt.x = (fb_w > prompt.w) ? (fb_w - prompt.w) : 0;
  if (prompt.y + prompt.h > fb_h - TASKBAR_PX)
    prompt.y =
        ((fb_h - TASKBAR_PX) > prompt.h) ? (fb_h - TASKBAR_PX - prompt.h) : 0;
}

void prompt_rect(int *px, int *py, int *pw, int *ph) {
  *px = prompt.x;
  *py = prompt.y;
  *pw = prompt.w;
  *ph = prompt.h;
}

void prompt_btn_rect(int idx, int *bx, int *by, int *bw, int *bh) {
  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  int n = prompt_button_count(prompt.kind);
  int row_w = n * PROMPT_BTN_W + (n - 1) * PROMPT_BTN_GAP;

  *bw = PROMPT_BTN_W;
  *bh = PROMPT_BTN_H;
  *bx = px + (pw - row_w) / 2 + idx * (PROMPT_BTN_W + PROMPT_BTN_GAP);
  *by = py + ph - PROMPT_PAD - PROMPT_BTN_H;
}

void prompt_send_result(int result) {
  if (!prompt.active)
    return;

  struct ipc_msg resp;
  memset(&resp, 0, sizeof(resp));
  resp.type = IPC_WM_PROMPT_RESP;
  resp.a = result;
  if (prompt.kind == WM_PROMPT_TEXT && result == WM_PROMPT_OK) {
    size_t i = 0;
    while (i < sizeof(resp.str) - 1 && prompt.input[i]) {
      resp.str[i] = prompt.input[i];
      i++;
    }
    resp.str[i] = 0;
  }
  ipc_send(prompt.owner_pid, &resp);

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  prompt.active = 0;
  /* The dialog covered whatever was beneath it, so damage its whole rect. */
  mark_dirty(px, py, pw, ph);
}

void handle_prompt_req(int owner_pid, int handle, int kind,
                              const char *message) {
  if (kind != WM_PROMPT_MESSAGE && kind != WM_PROMPT_CONFIRM &&
      kind != WM_PROMPT_TEXT)
    return;

  if (prompt.active) {
    /* Refuse rather than queue , see the note above. */
    struct ipc_msg busy;
    memset(&busy, 0, sizeof(busy));
    busy.type = IPC_WM_PROMPT_RESP;
    busy.a = WM_PROMPT_CANCEL;
    ipc_send(owner_pid, &busy);
    return;
  }

  memset(&prompt, 0, sizeof(prompt));
  prompt.active = 1;
  prompt.kind = kind;
  prompt.owner_pid = owner_pid;
  prompt.owner_handle = handle;
  prompt.selected = 0;
  if (message) {
    size_t i = 0;
    while (i < sizeof(prompt.message) - 1 && message[i]) {
      prompt.message[i] = message[i];
      i++;
    }
    prompt.message[i] = 0;
  }

  /* Freeze the rect while the owner window still exists. */
  prompt_freeze_rect();

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  mark_dirty(px, py, pw, ph);
  printf("winman: prompt kind=%d owner=%d handle=%d at %d,%d\n", kind,
         owner_pid, handle, px, py);
}

/* An owner that dies or loses its window mid-dialog leaves nothing to answer
 * to, so drop the dialog rather than leave it stranded on screen. */
void prompt_abandon_for_owner(int owner_pid) {
  if (!prompt.active || prompt.owner_pid != owner_pid)
    return;

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  prompt.active = 0;
  mark_dirty(px, py, pw, ph);
}

void draw_prompt(void) {
  if (!prompt.active)
    return;

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  if (!clip_hits(px, py, pw, ph))
    return;

  fb_fill_rect(px, py, pw, ph, PROMPT_BG);
  /* Frame: light top/left, dark bottom/right , the same raised look the
   * taskbar buttons use. */
  fb_fill_rect(px, py, pw, 1, CHROME_TEXT);
  fb_fill_rect(px, py, 1, ph, CHROME_TEXT);
  fb_fill_rect(px, py + ph - 1, pw, 1, TITLEBAR_BG);
  fb_fill_rect(px + pw - 1, py, 1, ph, TITLEBAR_BG);

  draw_text_fb(px + PROMPT_PAD, py + PROMPT_PAD, prompt.message,
               pw - 2 * PROMPT_PAD, PROMPT_TEXT, PROMPT_BG);

  if (prompt.kind == WM_PROMPT_TEXT) {
    int fx = px + PROMPT_PAD;
    int fy = py + PROMPT_PAD + FONT_GLYPH_H + PROMPT_PAD;
    int fw = pw - 2 * PROMPT_PAD;
    fb_fill_rect(fx, fy, fw, PROMPT_FIELD_H, PROMPT_FIELD_BG);
    fb_fill_rect(fx, fy, fw, 1, TITLEBAR_BG);
    fb_fill_rect(fx, fy, 1, PROMPT_FIELD_H, TITLEBAR_BG);

    int ty = fy + (PROMPT_FIELD_H - FONT_GLYPH_H) / 2;
    draw_text_fb(fx + 3, ty, prompt.input, fw - 6, PROMPT_TEXT,
                 PROMPT_FIELD_BG);
    /* Caret sits after the last glyph; the field scrolls nowhere, so a
     * string longer than the field just runs under the right edge. */
    int caret_x = fx + 3 + prompt.caret * FONT_GLYPH_W;
    if (caret_x < fx + fw - 2)
      fb_fill_rect(caret_x, ty, 1, FONT_GLYPH_H, PROMPT_TEXT);
  }

  int n = prompt_button_count(prompt.kind);
  for (int i = 0; i < n; i++) {
    int bx, by, bw, bh;
    prompt_btn_rect(i, &bx, &by, &bw, &bh);
    int sel = (i == prompt.selected);
    uint32_t bg = sel ? PROMPT_BTN_SEL_BG : PROMPT_BTN_BG;
    uint32_t fg = sel ? PROMPT_BTN_SEL_FG : PROMPT_TEXT;
    fb_fill_rect(bx, by, bw, bh, bg);
    fb_fill_rect(bx, by, bw, 1, CHROME_TEXT);
    fb_fill_rect(bx, by + bh - 1, bw, 1, TITLEBAR_BG);

    const char *label = prompt_button_label(prompt.kind, i);
    int label_w = (int)strlen(label) * FONT_GLYPH_W;
    draw_text_fb(bx + (bw - label_w) / 2, by + (bh - FONT_GLYPH_H) / 2, label,
                 bw, fg, bg);
  }
}

/* Returns 1 when the dialog consumed the event. Every key and every click
 * is consumed while a prompt is up , that is what modal means. */
int prompt_handle_key(int keycode, int shift) {
  if (!prompt.active)
    return 0;

  int n = prompt_button_count(prompt.kind);

  if (keycode == KEY_ESC) {
    prompt_send_result(WM_PROMPT_CANCEL);
    return 1;
  }
  if (keycode == KEY_ENTER || keycode == KEY_KPENTER) {
    prompt_send_result(prompt_button_result(prompt.kind, prompt.selected));
    return 1;
  }
  if (keycode == KEY_TAB || keycode == KEY_RIGHT) {
    prompt.selected = (prompt.selected + 1) % n;
    goto redraw;
  }
  if (keycode == KEY_LEFT) {
    prompt.selected = (prompt.selected + n - 1) % n;
    goto redraw;
  }

  if (prompt.kind == WM_PROMPT_TEXT) {
    if (keycode == KEY_BACKSPACE) {
      if (prompt.caret > 0)
        prompt.input[--prompt.caret] = 0;
      goto redraw;
    }
    char c = keymap_to_ascii(keycode, shift);
    if (c >= 32 && c <= 126 && prompt.caret < PROMPT_MAX_TEXT) {
      prompt.input[prompt.caret++] = c;
      prompt.input[prompt.caret] = 0;
      goto redraw;
    }
  }
  /* Swallow anything else: a stray key must not reach the app behind. */
  return 1;

redraw: {
  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  mark_dirty(px, py, pw, ph);
}
  return 1;
}

int prompt_handle_click(int mx, int my) {
  if (!prompt.active)
    return 0;

  int n = prompt_button_count(prompt.kind);
  for (int i = 0; i < n; i++) {
    int bx, by, bw, bh;
    prompt_btn_rect(i, &bx, &by, &bw, &bh);
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
      prompt_send_result(prompt_button_result(prompt.kind, i));
      return 1;
    }
  }
  /* Clicks elsewhere are eaten too, so the window underneath cannot be
   * focused, dragged, or closed while the dialog is waiting. */
  return 1;
}
