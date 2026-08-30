#define WINMAN_DECLARE_STATE
#include "winman.h"
#include "key_codes.h"
#include "syscall.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static int alt_pressed = 0;

/* Double-click tracking */
static int last_icon_clicked = -1;
static u32 last_icon_click_tick = 0;
static int last_title_clicked = -1;
static u32 last_title_click_tick = 0;
static int last_title_click_x = 0;
static int last_title_click_y = 0;

/* Close escalation. The close button is a request , the owner is expected to
 * tear its own window down so it can prompt about unsaved work first. An app
 * that ignores WM_EV_QUIT or has wedged would otherwise be unclosable, so a
 * second click while the first request is still outstanding destroys the
 * window here instead of asking again.
 *
 * The window is bounded in time as well as cleared on destroy: handles are
 * slot indices and get reused, so without a deadline a stale pending request
 * could force-destroy an unrelated window that later inherited the handle.
 * Ticks come from the PIT at 500 Hz, so this is roughly three seconds. */
#define CLOSE_ESCALATE_TICKS 1500u
int close_pending_handle = -1;
u32 close_pending_tick = 0;


void reset_titlebar_click(void) {
  last_title_clicked = -1;
  last_title_click_tick = 0;
}

/* Consume a second click on the same titlebar when it is close enough in both
 * time and position. Reset after a match so a triple-click only toggles once.
 */
int titlebar_click_is_double(int handle, int x, int y, uint32_t now) {
  int dx = x - last_title_click_x;
  int dy = y - last_title_click_y;
  if (dx < 0)
    dx = -dx;
  if (dy < 0)
    dy = -dy;

  if (handle == last_title_clicked &&
      now - last_title_click_tick < DOUBLE_CLICK_TICKS &&
      dx <= DOUBLE_CLICK_SLOP && dy <= DOUBLE_CLICK_SLOP) {
    reset_titlebar_click();
    return 1;
  }

  last_title_clicked = handle;
  last_title_click_tick = now;
  last_title_click_x = x;
  last_title_click_y = y;
  return 0;
}


void handle_set_status(int owner_pid, int handle, const char *text) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid != owner_pid || !text)
    return;
  /* Dropped for a window with no strip: there is nowhere to draw it, and
   * silently growing the frame would move the client area out from under a
   * client that never asked for one. */
  if (w->status_h <= 0)
    return;

  size_t i = 0;
  while (i < sizeof(w->status) - 1 && text[i]) {
    w->status[i] = text[i];
    i++;
  }
  w->status[i] = 0;
  /* Only the strip changed, so damage just it rather than the whole frame. */
  mark_dirty(w->x + BORDER_PX, w->y + TITLEBAR_PX + w->client_h,
             outer_w(w) - 2 * BORDER_PX, w->status_h);
}

void send_create_resp(int target_pid, int handle, uint64_t va,
                             uint32_t pitch, int w, int h) {
  struct ipc_msg resp;
  memset(&resp, 0, sizeof(resp));
  resp.type = IPC_WM_CREATE_RESP;
  resp.a = handle;
  resp.b = w;
  resp.c = h;
  resp.va = va;
  resp.pitch = pitch;
  ipc_send(target_pid, &resp);
}

void pump_ipc(void) {
  struct ipc_msg m;
  while (ipc_recv(&m)) {
    int from = (int)m.from_pid;
    switch (m.type) {
    case IPC_WM_CREATE_REQ: {
      uint64_t va = 0;
      uint32_t pitch = 0;
      int handle = 0;
      int rc =
          handle_create(from, m.a, m.b, m.str, m.flags, &va, &pitch, &handle);
      if (rc == 0) {
        send_create_resp(from, handle, va, pitch, m.a, m.b);
      } else {
        send_create_resp(from, -1, 0, 0, m.a, m.b);
      }
      break;
    }
    case IPC_WM_DESTROY_REQ:
      handle_destroy(from, m.a);
      break;
    case IPC_WM_INVALIDATE_REQ: {
      struct window *win = find_handle(m.a);
      if (win && win->owner_pid == from)
        mark_dirty(win->x, win->y, outer_w(win), outer_h(win));
      break;
    }
    case IPC_WM_SET_TITLE_REQ:
      handle_set_title(from, m.a, m.str);
      break;
    case IPC_WM_SET_STATUS_REQ:
      handle_set_status(from, m.a, m.str);
      break;
    case IPC_WM_PROMPT_REQ:
      handle_prompt_req(from, m.a, m.b, m.str);
      break;
    case IPC_PEER_EXITED: {
      /* Accept both peer-exit message layouts. */
      int dead_pid = (int)m.from_pid;
      if (dead_pid <= 0)
        dead_pid = (int)m.a;
      prompt_abandon_for_owner(dead_pid);
      destroy_windows_for_owner(dead_pid, "peer-exited");
      break;
    }
    default:
      break;
    }
  }
}

/* Translate input event from screen coords to the focused window's
 * client-area coords before forwarding. Clients draw into a surface that
 * starts at (0,0) so they shouldn't have to know their own position on
 * the desktop , winman is the only thing that does. */
void forward_input(int target_pid, int win_handle, const struct msg *m) {
  if (target_pid <= 0)
    return;

  struct ipc_msg out;
  memset(&out, 0, sizeof(out));
  out.type = IPC_WM_INPUT;
  out.a = m->type;

  if (m->type == MSG_MOUSE_MOVE || m->type == MSG_MOUSE_DOWN ||
      m->type == MSG_MOUSE_UP) {
    int wx = 0, wy = 0, wcw = 0, wch = 0;
    int local_x = m->x;
    int local_y = m->y;

    if (win_get_rect(win_handle, &wx, &wy, &wcw, &wch)) {
      local_x = m->x - (wx + BORDER_PX);
      local_y = m->y - (wy + TITLEBAR_PX);
    }

    out.b = m->param; /* Mouse button state/mask */
    out.c = local_x;
    out.d = local_y;
  } else {
    /* Keyboard event: pass key parameters cleanly without transforming mouse
     * coords */
    out.b = m->param; /* Key code / ASCII value */
    out.c = m->x; /* Pass auxiliary key details (e.g. key flags or scancodes) if
                     set */
    out.d = m->y;
  }

  ipc_send(target_pid, &out);
}

void request_window_close(int handle, u32 now) {
  /* A console has no client to ask politely: winman owns the window and
   * started the shell, so the close button acts immediately. Nothing here
   * needs the kernel to stay alive , the init task no longer waits on any
   * shell , so closing the last one is allowed. */
  struct console *c = con_for_handle(handle);
  if (c) {
    printf("winman: close button -> console handle=%d tty=%d\n", handle,
           c->tty);
    console_close(c);
    return;
  }

  struct window *w = find_handle(handle);
  if (!w || w->owner_pid <= 0)
    return;

  /* Second click on a window whose close request is still outstanding: the
   * owner had its chance to exit cleanly and did not take it, so take the
   * window away. Passing 0 for the pid check makes this unconditional ,
   * the owner is not the one asking. */
  if (close_pending_handle == handle &&
      now - close_pending_tick < CLOSE_ESCALATE_TICKS) {
    printf("winman: close button -> force handle=%d owner=%d\n", handle,
           w->owner_pid);
    close_pending_handle = -1;
    handle_destroy_internal(handle, 0);
    return;
  }

  printf("winman: close button -> request handle=%d\n", handle);
  close_pending_handle = handle;
  close_pending_tick = now;

  struct ipc_msg out;
  memset(&out, 0, sizeof(out));
  out.type = IPC_WM_INPUT;
  out.a = WM_EV_QUIT;
  ipc_send(w->owner_pid, &out);
}

void pump_input(void) {
  struct msg m;

  while (msg_get(&m)) {
    int forward = 1;

    /*
     * Modal prompt. Takes precedence over everything below , including an
     * in-flight drag , so no window can be moved, focused, or closed while
     * a dialog is waiting for an answer, and no keystroke leaks to the app
     * behind it. Mouse motion still falls through so the cursor keeps
     * tracking.
     */
    if (prompt.active) {
      if (m.type == MSG_KEY_DOWN) {
        int shift = (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT);
        if (shift)
          shift_held = 1;
        prompt_handle_key(m.param, shift_held);
        continue;
      }
      if (m.type == MSG_KEY_UP) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT)
          shift_held = 0;
        continue;
      }
      if (m.type == MSG_MOUSE_DOWN) {
        prompt_handle_click(m.x, m.y);
        continue;
      }
      if (m.type == MSG_MOUSE_UP)
        continue;
      /* MSG_MOUSE_MOVE falls through to the cursor tracking below. */
    }

    /*
     * Active move/resize operation.
     */
    if (drag.active) {
      if (m.type == MSG_MOUSE_DOWN || m.type == MSG_MOUSE_MOVE) {
        forward = 0;
      } else if (m.type == MSG_MOUSE_UP) {
        int dx = m.x - drag.grab_mx;
        int dy = m.y - drag.grab_my;

        if (drag.kind == HIT_TITLEBAR &&
            (dx < -DOUBLE_CLICK_SLOP || dx > DOUBLE_CLICK_SLOP ||
             dy < -DOUBLE_CLICK_SLOP || dy > DOUBLE_CLICK_SLOP))
          reset_titlebar_click();

        int old_x, old_y, old_cw, old_ch;
        if (win_get_rect(drag.handle, &old_x, &old_y, &old_cw, &old_ch)) {
          mark_dirty(old_x, old_y, outer_w_dims(old_cw),
                     outer_h_dims(old_ch, status_h_of(drag.handle)));
        }

        if (drag.kind == HIT_TITLEBAR) {
          int nx = drag.orig_x + dx;
          int ny = drag.orig_y + dy;

          clamp_to_desktop(&nx, &ny, drag.orig_cw, drag.orig_ch);

          win_set_pos(drag.handle, nx, ny);
        } else if (drag.kind == HIT_GRIP) {
          int new_cw = drag.orig_cw + dx;
          int new_ch = drag.orig_ch + dy;

          client_window_resize(drag.handle, new_cw, new_ch);
        }

        drag.active = 0;

        int new_x, new_y, new_cw, new_ch;
        if (win_get_rect(drag.handle, &new_x, &new_y, &new_cw, &new_ch)) {
          mark_dirty(new_x, new_y, outer_w_dims(new_cw),
                     outer_h_dims(new_ch, status_h_of(drag.handle)));
        }

        forward = 0;
      }
    }

    /*
     * Begin processing a left mouse click.
     */
    else if (m.type == MSG_MOUSE_DOWN && m.param == MOUSE_BTN_LEFT) {
      int hit_handle = 0;
      int kind = hit_test_at(m.x, m.y, &hit_handle);
      int titlebar_double_click = 0;
      if (start_menu_open && kind != HIT_START_MENU && kind != HIT_START_BTN) {
        start_menu_open = 0;
        start_menu_hover = -1;
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h);
        continue;
      }

      if (kind == HIT_TITLEBAR) {
        titlebar_double_click =
            titlebar_click_is_double(hit_handle, m.x, m.y, (u32)m.when);
      } else {
        reset_titlebar_click();
      }

      if (kind == HIT_BTN_CLOSE) {
        request_window_close(hit_handle, (u32)m.when);
        continue;
      }

      if (kind == HIT_BTN_MAX) {
        toggle_maximize(hit_handle);
        continue;
      }

      if (kind == HIT_BTN_MIN) {
        toggle_minimize(hit_handle);

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
        continue;
      }

      if (kind == HIT_START_BTN) {
        /* Damage the rect the menu currently occupies before rebuilding: if
         * the rescan changes the entry count the old, taller rect still has
         * to be repainted or its bottom rows are left on screen. */
        int old_menu_h =
            start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - old_menu_h, START_MENU_W,
                   old_menu_h + TASKBAR_PX);

        start_menu_open = !start_menu_open;
        start_menu_hover = -1;

        /* Rescan on open rather than per-frame: the directories can gain a
         * binary while winman runs, but a scan per repaint would hit the
         * filesystem on every hover change. */
        // if (start_menu_open)
        //   build_start_menu_entries();

        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;

        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h + TASKBAR_PX);

        continue;
      }

      /*
       * Taskbar window button.
       */
      if (kind == HIT_TASKBAR_BTN) {
        int prev_focus = focused_handle;

        // if (hit_handle == HANDLE_CONSOLE) {
        //   /*
        //    * The console is not in windows[], so find_handle()
        //    * cannot be used for it.
        //    */
        //   focused_handle = HANDLE_CONSOLE;
        //   z_bring_to_front(HANDLE_CONSOLE);
        // } else
        {
          struct window *wt = find_handle(hit_handle);

          if (wt) {
            if (wt->minimized) {
              toggle_minimize(hit_handle);
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            } else if (focused_handle == hit_handle) {
              toggle_minimize(hit_handle);
              z_send_to_back(hit_handle);
            } else {
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            }

            mark_dirty(wt->x, wt->y, outer_w(wt), outer_h(wt));
          }
        }

        /* Repaint the old and new focus chrome. */
        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        continue;
      }

      if (kind == HIT_START_MENU) {
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;

        int menu_y = taskbar_y() - menu_h;
        int relative_y = m.y - menu_y - START_MENU_PAD;

        int clicked_item = relative_y / START_MENU_ITEM_H;

        if (clicked_item >= 0 && clicked_item < start_menu_count) {
          launch_program(start_menu_programs[clicked_item].path);
        }

        start_menu_open = false;
        start_menu_hover = -1;

        mark_dirty(0, menu_y, START_MENU_W, menu_h);

        mark_dirty(0, taskbar_y(), TASKBAR_START_W, TASKBAR_PX);

        continue;
      }

      if (kind == HIT_TITLEBAR || kind == HIT_GRIP) {
        int x, y, cw, ch;

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          int prev_focus = focused_handle;

          focused_handle = hit_handle;
          z_bring_to_front(hit_handle);

          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));

          if (prev_focus != hit_handle) {
            int prev_x, prev_y, prev_cw, prev_ch;
            if (win_get_rect(prev_focus, &prev_x, &prev_y, &prev_cw,
                             &prev_ch)) {
              mark_dirty(prev_x, prev_y, outer_w_dims(prev_cw),
                         outer_h_dims(prev_ch, status_h_of(prev_focus)));
            }
          }

          mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

          if (titlebar_double_click) {
            printf("winman: titlebar double-click handle=%d\n", hit_handle);
            toggle_maximize(hit_handle);
            forward = 0;
            continue;
          }

          drag.active = 1;
          drag.kind = kind;
          drag.handle = hit_handle;
          drag.grab_mx = m.x;
          drag.grab_my = m.y;
          drag.orig_x = x;
          drag.orig_y = y;
          drag.orig_cw = cw;
          drag.orig_ch = ch;
        }

        forward = 0;
      }

      /*
       * Click on the status strip. Chrome, so it focuses the window like
       * any other frame click but is never forwarded to the client.
       */
      else if (kind == HIT_STATUSBAR) {
        if (focused_handle != hit_handle) {
          int prev_focus = focused_handle;
          focused_handle = hit_handle;
          z_bring_to_front(hit_handle);

          int x, y, cw, ch;
          if (win_get_rect(hit_handle, &x, &y, &cw, &ch))
            mark_dirty(x, y, outer_w_dims(cw),
                       outer_h_dims(ch, status_h_of(hit_handle)));
          if (win_get_rect(prev_focus, &x, &y, &cw, &ch))
            mark_dirty(x, y, outer_w_dims(cw),
                       outer_h_dims(ch, status_h_of(prev_focus)));
          mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
        }
        forward = 0;
      }

      /*
       * Click inside a window's client area.
       */
      else if (kind == HIT_CLIENT) {
        int prev_focus = focused_handle;

        focused_handle = hit_handle;
        z_bring_to_front(hit_handle);

        int x, y, cw, ch;

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));
        }

        if (prev_focus != hit_handle &&
            win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        /*
         * A console consumes its input locally.
         */
        if (is_console_handle(hit_handle))
          forward = 0;
      }

      /*
       * Desktop background or desktop icon.
       */
      else if (kind == HIT_NONE) {
        int clicked_icon = -1;

        for (int i = 0; i < desktop_icon_count; i++) {
          if (m.x >= desktop_icons[i].x &&
              m.x < desktop_icons[i].x + desktop_icons[i].w &&
              m.y >= desktop_icons[i].y &&
              m.y < desktop_icons[i].y + desktop_icons[i].h) {
            clicked_icon = i;
            break;
          }
        }

        if (clicked_icon >= 0) {
          uint32_t current_tick = (uint32_t)m.when;

          printf("winman: clicked icon %d\n", clicked_icon);

          printf("winman: tick=%u, last_icon=%d, "
                 "last_tick=%u\n",
                 current_tick, last_icon_clicked, last_icon_click_tick);

          if (clicked_icon == last_icon_clicked &&
              current_tick - last_icon_click_tick < DOUBLE_CLICK_TICKS) {
            launch_program(desktop_icons[clicked_icon].program.path);

            /*
             * Prevent a triple-click from launching twice.
             */
            last_icon_clicked = -1;
            last_icon_click_tick = 0;
          } else {
            last_icon_clicked = clicked_icon;
            last_icon_click_tick = current_tick;
          }
        } else {
          /*
           * Clicking elsewhere invalidates the pending
           * desktop-icon double-click.
           */
          last_icon_clicked = -1;
          last_icon_click_tick = 0;
        }

        /*
         * Desktop clicks remove the current window focus.
         */
        int prev_focus = focused_handle;
        focused_handle = 0;

        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        forward = 0;
      }
    } else if (m.type == MSG_KEY_DOWN) {
      // Track Alt key state
      if (m.param == KEY_LEFTALT || m.param == KEY_RIGHTALT) {
        alt_pressed = 1;
      }

      /* Alt+F4 closes the focused window, console or client. It used to skip
       * the console because there was no way to close one; now there is. */
      if (m.param == KEY_F4 && alt_pressed && focused_handle > 0) {
        request_window_close(focused_handle, (u32)m.when);
        forward = 0; // Consume the event
        continue;
      }

      // Escape closes the start menu
      if (m.param == KEY_ESC && start_menu_open) {
        start_menu_open = 0;
        start_menu_hover = -1;
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h);
        forward = 0; // Consume the event
        continue;
      }
    }

    /*
     * Track key releases to update modifier state.
     */
    else if (m.type == MSG_KEY_UP) {
      if (m.param == KEY_LEFTALT || m.param == KEY_RIGHTALT) {
        alt_pressed = 0;
      }
    }

    /*
     * Consume events handled by the window manager.
     */
    if (!forward)
      continue;

    /*
     * Console input is handled by the window manager rather than
     * forwarded to a client process. Which console gets the keystroke is
     * decided by focus, and it is injected into that console's own TTY
     * channel , which is what keeps two shells from reading each other's
     * typing.
     */
    struct console *focus_con = con_focused();
    if (focus_con) {
      if (m.type == MSG_KEY_DOWN) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT) {
          shift_held = 1;
        }
        if (m.param == KEY_LEFTCTRL || m.param == KEY_RIGHTCTRL) {
          ctrl_held = 1;
        }

        /* Console zoom lives here rather than in the shell. The shell now
         * receives ASCII from the TTY ring and never sees a raw keycode, so
         * it cannot spot Ctrl+-/Ctrl+= any more , and winman owns the
         * console's scale anyway. Scale is per-console: zooming one leaves
         * the others alone. */
        if (ctrl_held && m.param == KEY_MINUS) {
          con_set_scale(focus_con, focus_con->scale - 1);
          continue;
        }
        if (ctrl_held && m.param == KEY_EQUAL) {
          con_set_scale(focus_con, focus_con->scale + 1);
          continue;
        }

        char c = keymap_to_ascii(m.param, shift_held);
        if (c != 0) {
          tty_inject(focus_con->tty, c);
        }
      } else if (m.type == MSG_KEY_UP) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT) {
          shift_held = 0;
        }
        if (m.param == KEY_LEFTCTRL || m.param == KEY_RIGHTCTRL) {
          ctrl_held = 0;
        }
      }
      continue;
    }

    /*
     * Forward unhandled input to the focused client window.
     */
    struct window *focus = focused_handle ? find_handle(focused_handle) : 0;

    if (focus) {
      forward_input(focus->owner_pid, focus->handle, &m);
    }
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("winman: built " __DATE__ " " __TIME__ "\n");

  printf("winman: start\n");
  if (wm_register() != 0) {
    printf("winman: wm_register failed\n");
    return 1;
  }
  printf("winman: registered\n");

  struct fb_info info;
  if (fb_info(&info) != 0)
    return 2;
  fb_hw = (uint32_t *)fb_map();
  if (!fb_hw)
    return 3;

  fb_w = (int)info.width;
  fb_h = (int)info.height;
  fb_stride = (int)(info.pitch / 4);
  fb_bytes = info.pitch * info.height;
  fb_mapped_bytes = fb_bytes;
  update_client_size_limits();
  printf("winman: fb %dx%d pitch=%d bytes=%d\n", fb_w, fb_h, fb_stride * 4,
         (int)fb_bytes);

  if (backbuffer_reserve(fb_bytes) != 0) {
    printf("winman: back buffer alloc failed\n");
    return 4;
  }
  if (backbuffer_register() != 0)
    printf("winman: back buffer registration failed, using fallback\n");
  printf("winman: back buffer @%p bytes=%d\n", (void *)fb, (int)fb_capacity);

  cursor_load();
  tb_load_start_icon();
  desktop_load();
  /* After fb_h is known , the menu's capacity depends on the screen height. */
  build_start_menu_entries();
  /* Populate before the first compose so the taskbar paints a real time
   * instead of a blank strip that fills in a second later. */
  clock_format();

  memset(windows, 0, sizeof(windows));
  focused_handle = 0;
  /* The boot console. Its shell is started from here rather than by the
   * kernel so winman owns the pid and the close button has something to
   * kill; the kernel only runs a shell of its own when no WM registers. */
  struct console *boot_con = console_open();
  if (boot_con)
    printf("winman: console tty=%d surface=%p w=%d h=%d\n", boot_con->tty,
           (void *)boot_con->win.surface, boot_con->win.client_w,
           boot_con->win.client_h);
  else
    printf("winman: boot console failed to open\n");
  present_full_desktop();
  printf("winman: ready\n");

  int self_pid = (int)get_pid();
  int tick = 0;
  int32_t last_cx = 0, last_cy = 0;
  int have_last = 0;

  for (;;) {
    if ((int)wm_pid() != self_pid) {
      /* Another process may replace the kernel's single display registration
       * while it owns the framebuffer. Re-establish ours on return. */
      fb_registered = 0;
      have_last = 0;
      sleep_ticks(1);
      continue;
    }
    if (!fb_registered && backbuffer_register() != 0)
      sleep_ticks(1);

    /* Host-driven resize: kernel may have re-pointed the scanout at a
     * different-sized backing under us. Re-query dims; on change, rebind
     * USER_FB_BASE (sys_fb_map walks the new scatter-gather page list)
     * and grow/shrink our back buffer to match. */
    {
      struct fb_info cur;
      if (fb_info(&cur) == 0 &&
          ((int)cur.width != fb_w || (int)cur.height != fb_h ||
           (int)(cur.pitch / 4) != fb_stride)) {
        size_t new_bytes = cur.pitch * cur.height;
        uint32_t *new_fb_hw = fb_hw;

        /* The physical pool is stable. Only growing beyond the prefix this
         * process already mapped needs another fb_map syscall. */
        if (new_bytes > fb_mapped_bytes) {
          new_fb_hw = (uint32_t *)fb_map();
          if (new_fb_hw)
            fb_mapped_bytes = new_bytes;
        }

        if (new_fb_hw && backbuffer_reserve(new_bytes) == 0) {
          fb_hw = new_fb_hw;
          fb_w = (int)cur.width;
          fb_h = (int)cur.height;
          fb_stride = (int)(cur.pitch / 4);
          fb_bytes = new_bytes;
          update_client_size_limits();
          if (backbuffer_register() != 0)
            printf("winman: resized back buffer registration failed\n");
          present_full_desktop();
          have_last = 0;
          printf("winman: rebound fb to %dx%d\n", fb_w, fb_h);
        }
      }
    }

    pump_ipc();
    pump_input();
    drain_tty_into_console();

    /* The taskbar is outside most dirty boxes, so nothing else would repaint
     * it. Track what it renders from and dirty the strip when that changes,
     * otherwise a new window's button never appears. */
    {
      static uint64_t taskbar_sig_prev;
      uint64_t sig = (uint64_t)(uint32_t)focused_handle * 1000003u;
      for (int i = 0; i < CON_MAX; i++)
        sig = sig * 31 + (uint64_t)cons[i].win.in_use;
      for (int i = 0; i < MAX_WINDOWS; i++) {
        sig = sig * 31 + (uint64_t)windows[i].in_use;
        if (!windows[i].in_use)
          continue;
        sig = sig * 31 + (uint64_t)windows[i].handle;
        sig = sig * 31 + (uint64_t)windows[i].minimized;
        for (const char *t = windows[i].title; *t; t++)
          sig = sig * 31 + (unsigned char)*t;
      }
      if (sig != taskbar_sig_prev) {
        taskbar_sig_prev = sig;
        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
      }
    }

    /* Advance once per iteration, not once per repaint. This used to
     * used to live inside a damage-dependent test below, where
     * short-circuit evaluation froze it on an idle desktop , turning
     * the reap check into a constant that either fired every frame or
     * never fired at all, depending on where it stopped. */
    tick++;

    /* Reap windows whose owners have died without sending DESTROY_REQ.
     * Hot path runs the syscall (proc_list) ~every 64 ticks so the
     * common case stays cheap. */
    if ((tick & 63) == 0)
      reap_dead_windows();

    /* Clock. Polls the RTC on an interval rather than every frame, and only
     * damages the strip when the rendered text differs , an idle desktop
     * stays idle for the 59 seconds when nothing has changed. */
    if ((uint32_t)tick % CLOCK_POLL_TICKS == 0)
      clock_tick();

    int32_t mx, my;
    uint8_t btns;
    mouse_pos(&mx, &my, &btns);
    (void)btns;

    /* Drag-in-flight fast path: skip compose entirely and just maintain
     * the ghost outline + cursor on top of the frozen last-composed
     * frame. fb (back buffer) is untouched so the strips we lift from
     * it during erase_ghost are still the right pixels. */
    if (drag.active) {
      if (drag.have_ghost) {
        erase_ghost(drag.last_gx, drag.last_gy, drag.last_gw, drag.last_gh);
      }
      int cursor_moved = have_last && (mx != last_cx || my != last_cy);
      int cursor_refresh = !have_last || cursor_moved || ((tick & 127) == 0);
      if (cursor_moved)
        present_cursor_repair_at(last_cx, last_cy);
      int gx, gy, gw, gh;
      compute_ghost((int)mx, (int)my, &gx, &gy, &gw, &gh);
      draw_ghost(gx, gy, gw, gh);
      drag.last_gx = gx;
      drag.last_gy = gy;
      drag.last_gw = gw;
      drag.last_gh = gh;
      drag.have_ghost = 1;

      if (cursor_refresh)
        draw_cursor(mx, my);
      last_cx = mx;
      last_cy = my;
      have_last = 1;
      yield();
      continue;
    }

    /* Drag just ended this tick , wipe the lingering ghost outline
     * before the normal compose path runs. Damage was recorded by
     * MOUSE_UP so the next branch will repaint everything anyway. */
    if (drag.have_ghost) {
      erase_ghost(drag.last_gx, drag.last_gy, drag.last_gw, drag.last_gh);
      drag.have_ghost = 0;
    }

    int recompose = gfx_damage_pending(&desktop_damage);
    if (recompose) {
      present_dirty();
    }

    int cursor_moved = have_last && (mx != last_cx || my != last_cy);
    struct fb_rect cursor_repairs[1];
    uint32_t cursor_repair_count = 0;
    if (cursor_moved &&
        cursor_rect(last_cx, last_cy, cursor_scale(), &cursor_repairs[0]))
      cursor_repair_count = 1;
    if (!have_last || recompose || cursor_moved)
      draw_cursor_with_repairs(mx, my, cursor_repairs, cursor_repair_count);
    last_cx = mx;
    last_cy = my;
    have_last = 1;

    yield();
  }
  return 0;
}
