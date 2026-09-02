#define WINMAN_DECLARE_STATE
#include "key_codes.h"
#include "syscall.h"
#include "winman.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static int max_client_w = MIN_CLIENT_W;
static int max_client_h = MIN_CLIENT_H;
static char trunc_titles[MAX_WINDOWS][TASKBAR_BTN_W + 1];

/* Recompute the largest client area that can fit above the taskbar. Keep the
 * allocator's existing hard limit as a second ceiling, and retain the minimum
 * valid chrome dimensions for unusually small framebuffer modes. */
void update_client_size_limits(void) {
  int new_max_w = fb_w - 2 * BORDER_PX;
  int new_max_h = fb_h - TASKBAR_PX - TITLEBAR_PX - BORDER_PX;

  if (new_max_w > CLIENT_DIM_HARD_LIMIT)
    new_max_w = CLIENT_DIM_HARD_LIMIT;
  if (new_max_h > CLIENT_DIM_HARD_LIMIT)
    new_max_h = CLIENT_DIM_HARD_LIMIT;
  if (new_max_w < MIN_CLIENT_W)
    new_max_w = MIN_CLIENT_W;
  if (new_max_h < MIN_CLIENT_H)
    new_max_h = MIN_CLIENT_H;

  max_client_w = new_max_w;
  max_client_h = new_max_h;
}

void clamp_client_size(int *client_w, int *client_h) {
  if (*client_w < MIN_CLIENT_W)
    *client_w = MIN_CLIENT_W;
  if (*client_h < MIN_CLIENT_H)
    *client_h = MIN_CLIENT_H;
  if (*client_w > max_client_w)
    *client_w = max_client_w;
  if (*client_h > max_client_h)
    *client_h = max_client_h;
}

int win_get_rect(int handle, int *x, int *y, int *cw, int *ch) {
  if (is_console_handle(handle)) {
    struct console *c = con_for_handle(handle);
    if (!c)
      return 0;
    *x = c->win.x;
    *y = c->win.y;
    *cw = c->win.client_w;
    *ch = c->win.client_h;
    return 1;
  }
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      *x = windows[i].x;
      *y = windows[i].y;
      *cw = windows[i].client_w;
      *ch = windows[i].client_h;
      return 1;
    }
  }
  return 0;
}

void win_set_pos(int handle, int x, int y) {
  if (is_console_handle(handle)) {
    struct console *c = con_for_handle(handle);
    if (c) {
      c->win.x = x;
      c->win.y = y;
    }
    return;
  }
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      windows[i].x = x;
      windows[i].y = y;
      return;
    }
  }
}

int outer_w_dims(int cw) { return cw + 2 * BORDER_PX; }

/* Outer height from a client height plus that window's status strip. The
 * status height is an explicit parameter rather than looked up inside,
 * because most callers already hold the window and the ones that don't must
 * be made to say which window they mean , a frame height that silently
 * omits the strip leaves it unpainted or untestable. */
int outer_h_dims(int ch, int status_h) {
  return ch + TITLEBAR_PX + BORDER_PX + status_h;
}

/* Status height for a handle, 0 if it has no window or no strip. For the
 * call sites that only carry a handle. */
int status_h_of(int handle) {
  struct window *w = find_handle(handle);
  return w ? w->status_h : 0;
}

/* True iff (mx,my) lies inside titlebar button `idx_from_right` for a window
 * whose outer rect is (win_x, win_y, outer_w, _). Used by hit_test_at to
 * carve close/min/max regions out of HIT_TITLEBAR before returning. */
int in_titlebar_btn(int win_x, int win_y, int outer_w, int idx_from_right,
                    int mx, int my) {
  int bx, by, bw, bh;
  titlebar_btn_rect(win_x, win_y, outer_w, idx_from_right, &bx, &by, &bw, &bh);
  return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

/* Enumerate the windows that should appear on the taskbar in stable order:
 * the consoles first, in channel order, then in-use client windows in their
 * array slot order. Order matches z-order today since neither has explicit
 * raising. */
int build_taskbar_entries(struct tb_entry *out, int max) {
  int n = 0;
  for (int i = 0; i < CON_MAX && n < max; i++) {
    if (!cons[i].win.in_use)
      continue;
    out[n].handle = HANDLE_CONSOLE_BASE + i;
    out[n].title = cons[i].win.title;
    n++;
  }

  int max_chars = (TASKBAR_BTN_W - 8) / FONT_GLYPH_W;

  for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
    if (!windows[i].in_use)
      continue;
    out[n].handle = windows[i].handle;
    size_t title_len = strlen(windows[i].title);

    if ((int)title_len <= max_chars) {
      out[n].title = windows[i].title;
    } else {
      char *trunc = trunc_titles[i];
      strncpy(trunc, windows[i].title, max_chars - 3);
      trunc[max_chars - 3] = '\0';
      strcat(trunc, "...");
      out[n].title = trunc;
    }
    n++;
  }
  return n;
}

int taskbar_y(void) { return fb_h - TASKBAR_PX; }

/* Rightmost x a taskbar button may occupy. Without this the button strip
 * grows under the clock and paints over it. */
int taskbar_btn_limit(void) {
  return fb_w - CLOCK_W - CLOCK_PAD_R - TASKBAR_BTN_GAP;
}

/* Returns 1 + writes *out_handle when the click landed on a taskbar button,
 * 0 otherwise (including clicks on the taskbar strip background). Caller
 * should still treat strip clicks as "ate the input" , the strip is opaque
 * and never belongs to a client. */
int hit_taskbar(int mx, int my, int *out_handle) {
  int y = taskbar_y();
  if (my < y || my >= y + TASKBAR_PX)
    return 0;

  struct tb_entry ents[MAX_Z];
  int n = build_taskbar_entries(ents, (int)(sizeof(ents) / sizeof(ents[0])));

  int bx = TASKBAR_START_W + TASKBAR_BTN_GAP;
  int by = y;
  int bh = TASKBAR_PX;

  for (int i = 0; i < n; i++) {
    if (bx + TASKBAR_BTN_W > taskbar_btn_limit())
      break;
    if (mx >= bx && mx < bx + TASKBAR_BTN_W && my >= by && my < by + bh) {
      *out_handle = ents[i].handle;
      return 1;
    }
    bx += TASKBAR_BTN_W + TASKBAR_BTN_GAP;
  }
  return 0;
}

int in_start_menu(int menu_x, int menu_y, int mx, int my) {
  if (!start_menu_open)
    return 0;
  int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
  return mx >= menu_x && mx < menu_x + START_MENU_W && my >= menu_y &&
         my < menu_y + menu_h;
}

/* Classify a screen-space hit by walking the z-order topmost-first. The
 * console is just another z-stack entry now; whichever handle is at
 * z_order[0] wins ties at the same pixel. */
int hit_test_at(int mx, int my, int *out_handle) {
  if (start_menu_open) {
    int menu_x = 0;
    int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
    int menu_y = taskbar_y() - menu_h;
    if (in_start_menu(menu_x, menu_y, mx, my)) {
      return HIT_START_MENU;
    }
  }

  if (my >= taskbar_y()) {
    if (mx >= 0 && mx < TASKBAR_START_W) {
      return HIT_START_BTN;
    }
    int tb_handle = 0;
    if (hit_taskbar(mx, my, &tb_handle)) {
      *out_handle = tb_handle;
      return HIT_TASKBAR_BTN;
    }
    return HIT_NONE;
  }

  for (int i = 0; i < z_count; i++) {
    int h = z_order[i];
    if (is_minimized(h))
      continue;
    int x, y, cw, ch;
    if (!win_get_rect(h, &x, &y, &cw, &ch))
      continue;
    int ow = outer_w_dims(cw);
    int oh = outer_h_dims(ch, status_h_of(h));
    if (mx < x || mx >= x + ow || my < y || my >= y + oh)
      continue;
    *out_handle = h;
    /* Grip beats titlebar when they overlap at the seam. */
    if (mx >= x + ow - RESIZE_GRIP && my >= y + oh - RESIZE_GRIP)
      return HIT_GRIP;
    if (my < y + TITLEBAR_PX) {
      if (in_titlebar_btn(x, y, ow, 0, mx, my))
        return HIT_BTN_CLOSE;
      if (in_titlebar_btn(x, y, ow, 1, mx, my))
        return HIT_BTN_MAX;
      if (in_titlebar_btn(x, y, ow, 2, mx, my))
        return HIT_BTN_MIN;
      return HIT_TITLEBAR;
    }
    /* Below the client area is the status strip, not the client. Returning
     * HIT_CLIENT here would forward the click at a y past the bottom of the
     * client's surface. The grip check above already claimed the corner. */
    if (status_h_of(h) > 0 && my >= y + TITLEBAR_PX + ch)
      return HIT_STATUSBAR;
    return HIT_CLIENT;
  }
  return HIT_NONE;
}

void clamp_to_desktop(int *x, int *y, int cw, int ch) {
  int ow = outer_w_dims(cw);
  (void)ch;
  /* Keep at least the title bar reachable for re-drag. Bottom limit also
   * subtracts the taskbar so windows can't hide their title strip behind it. */
  if (*x + ow < TITLEBAR_PX)
    *x = TITLEBAR_PX - ow;
  if (*y < 0)
    *y = 0;
  if (*x > fb_w - TITLEBAR_PX)
    *x = fb_w - TITLEBAR_PX;
  if (*y > fb_h - TITLEBAR_PX - TASKBAR_PX)
    *y = fb_h - TITLEBAR_PX - TASKBAR_PX;
}

void client_window_resize(int handle, int new_cw, int new_ch) {
  clamp_client_size(&new_cw, &new_ch);

  if (is_console_handle(handle)) {
    console_resize(con_for_handle(handle), new_cw, new_ch);
    return;
  }

  struct window *w = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      w = &windows[i];
      break;
    }
  }
  if (!w) {
    printf("winman: client_window_resize: no window found for handle=%d\n",
           handle);
    return;
  }
  if (new_cw == w->client_w && new_ch == w->client_h)
    return;

  size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)page_aligned_alloc(pages, &raw);
  if (!surface)
    return;

  /* Preserve the upper-left intersection of the old surface , same idea
   * as console_resize. The client can't paint until it receives the
   * resize notify, so racing it for these reads is acceptable. */
  if (w->surface) {
    int copy_w = w->client_w < new_cw ? w->client_w : new_cw;
    int copy_h = w->client_h < new_ch ? w->client_h : new_ch;
    for (int y = 0; y < copy_h; y++) {
      memcpy(surface + (size_t)y * (size_t)new_cw,
             w->surface + (size_t)y * (size_t)w->client_w, (size_t)copy_w * 4);
    }
  }

  uint64_t client_va = 0;
  if (shmem_share(w->owner_pid, (uint64_t)surface, (long)pages, &client_va) !=
      0) {
    free(raw);
    return;
  }

  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));

  uint64_t old_client_va = w->client_va;
  int old_n_pages = w->n_pages;
  void *old_raw = w->surface_raw;

  w->surface = surface;
  w->surface_raw = raw;
  w->n_pages = (int)pages;
  w->client_va = client_va;
  w->client_w = new_cw;
  w->client_h = new_ch;

  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));

  struct ipc_msg note;
  memset(&note, 0, (sizeof(note)));
  note.type = IPC_WM_RESIZE_NOTIFY;
  note.a = handle;
  note.b = new_cw;
  note.c = new_ch;
  note.va = client_va;
  note.pitch = (uint32_t)(new_cw * 4);
  ipc_send(w->owner_pid, &note);

  // free old client
  if (old_client_va && old_n_pages > 0) {
    // unmap old client range w->owner_pid, old_client_va, old_n_pages
    shmem_unshare(w->owner_pid, old_client_va, old_n_pages);
  };

  if (old_raw)
    free(old_raw);
}

/* Compute the outer rect of the proposed drag target. For move drags the
 * size is fixed at the original; for resize drags the position is fixed and
 * the size grows/shrinks with the mouse delta. */
void compute_ghost(int mx, int my, int *gx, int *gy, int *gw, int *gh) {
  int dx = mx - drag.grab_mx;
  int dy = my - drag.grab_my;
  if (drag.kind == HIT_TITLEBAR) {
    int nx = drag.orig_x + dx;
    int ny = drag.orig_y + dy;
    clamp_to_desktop(&nx, &ny, drag.orig_cw, drag.orig_ch);
    *gx = nx;
    *gy = ny;
    *gw = outer_w_dims(drag.orig_cw);
    *gh = outer_h_dims(drag.orig_ch, status_h_of(drag.handle));
  } else {
    int ncw = drag.orig_cw + dx;
    int nch = drag.orig_ch + dy;
    clamp_client_size(&ncw, &nch);
    *gx = drag.orig_x;
    *gy = drag.orig_y;
    *gw = outer_w_dims(ncw);
    *gh = outer_h_dims(nch, status_h_of(drag.handle));
  }
}

struct window *find_slot(void) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use)
      return &windows[i];
  }
  return 0;
}

/* Consoles answer here too: their `struct window` carries the same geometry,
 * title and min/max state, so drag, minimize, maximize and the taskbar work
 * on them without a second code path. What is console-specific , the cell
 * grid, the TTY channel, the shell pid , lives in struct console and is
 * reached through con_for_handle instead. */
struct window *find_handle(int handle) {
  if (is_console_handle(handle)) {
    struct console *c = con_for_handle(handle);
    return c ? &c->win : NULL;
  }

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle)
      return &windows[i];
  }
  return NULL;
}

int handle_create(int client_pid, int w, int h, const char *title,
                  uint32_t flags, uint64_t *out_client_va, uint32_t *out_pitch,
                  int *out_handle) {
  if (w <= 0 || h <= 0 || w > CLIENT_DIM_HARD_LIMIT ||
      h > CLIENT_DIM_HARD_LIMIT)
    return -1;
  struct window *win = find_slot();
  if (!win) {
    printf("winman: handle_create: no slot found\n");
    return -1;
  }

  size_t pixel_bytes = (size_t)w * (size_t)h * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)page_aligned_alloc(pages, &raw);
  if (!surface)
    return -1;

  /* Map the same physical pages into the client's PML4 so the client
   * can write pixels without going through winman. */
  uint64_t client_va = 0;
  if (shmem_share(client_pid, (uint64_t)surface, (long)pages, &client_va) !=
      0) {
    free(raw);
    return -1;
  }

  win->in_use = 1;
  /* Handles remain stable until their slot is reused. */
  win->handle = (int)(win - windows) + 1;
  win->owner_pid = client_pid;
  /* Cascade new windows. */
  int placed = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (&windows[i] != win && windows[i].in_use)
      placed++;
  }
  win->x = 60 + placed * 24;
  win->y = 60 + placed * 24;
  win->client_w = w;
  win->client_h = h;
  win->surface = surface;
  win->surface_raw = raw;
  win->n_pages = (int)pages;
  win->client_va = client_va;
  if (title) {
    size_t i = 0;
    while (i < sizeof(win->title) - 1 && title[i]) {
      win->title[i] = title[i];
      i++;
    }
    win->title[i] = 0;
  } else {
    win->title[0] = 0;
  }

  /* Opt-in status strip. Set before the first mark_dirty below so the
   * initial damage rect covers the taller frame. */
  win->status_h = (flags & WM_CREATE_STATUSBAR) ? STATUSBAR_PX : 0;
  win->status[0] = 0;

  focused_handle = win->handle;
  z_bring_to_front(win->handle);
  mark_dirty(win->x, win->y, outer_w(win), outer_h(win));

  *out_client_va = client_va;
  *out_pitch = (uint32_t)(w * 4);
  *out_handle = win->handle;
  printf("winman: create handle=%d owner=%d %dx%d pages=%d client_va=%lx "
         "active=%d\n",
         win->handle, client_pid, w, h, (int)pages, (unsigned long)client_va,
         window_count());
  return 0;
}

/* Count of slots currently in_use. Used by the taskbar enumeration and
 * for debug logging on create/destroy/reap. */
int window_count(void) {
  int n = 0;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (windows[i].in_use)
      n++;
  return n;
}

/* Tear down a window. Owner-check by client_pid is skipped when
 * client_pid == 0 , used by the reaper for dead-client cleanup, since the
 * dead owner can no longer issue the destroy itself. */
void handle_destroy_internal(int handle, int client_pid_check) {
  /* Consoles look like windows to find_handle, but tearing one down that way
   * would free the surface and leave the cell grid, the alt-screen buffer and
   * the TTY channel behind. Route them to the path that knows about those. */
  struct console *c = con_for_handle(handle);
  if (c) {
    console_close(c);
    return;
  }

  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: handle_destroy_internal: no window found for handle=%d\n",
           handle);
    return;
  }
  if (client_pid_check && w->owner_pid != client_pid_check)
    return;

  /* Preserve cleanup state before clearing the window. */
  int old_x = w->x;
  int old_y = w->y;
  int old_ow = outer_w(w);
  int old_oh = outer_h(w);
  int old_owner_pid = w->owner_pid;
  uint64_t old_client_va = w->client_va;
  int old_n_pages = w->n_pages;
  void *old_surface_raw = w->surface_raw;

  /* Revoke the receiver mapping before making the owner allocation reusable.
   * A client may close one window and remain alive, so relying on process exit
   * to discard its PML4 leaves a stale alias into the next window allocated in
   * this heap block. If the owner is already gone, unshare fails harmlessly:
   * its address space has either been reaped or is about to be reaped. */
  if (old_client_va && old_n_pages > 0)
    shmem_unshare(old_owner_pid, old_client_va, old_n_pages);
  if (old_surface_raw)
    free(old_surface_raw);
  memset(w, 0, sizeof(*w));
  z_remove(handle);

  /* Drop any outstanding close request: this slot's handle is free for
   * reuse, and a stale pending entry would force-destroy whichever window
   * inherits it on the user's next single close click. */
  if (close_pending_handle == handle)
    close_pending_handle = -1;

  /* A dialog owned by this window has nothing left to answer to. */
  if (old_owner_pid > 0)
    prompt_abandon_for_owner(old_owner_pid);

  if (focused_handle == handle) {
    focused_handle = z_count > 0 ? z_order[0] : 0;

    /* Force taskbar redraw because the active button changed */
    mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

    /* Force new focused window to redraw its titlebar color */
    if (focused_handle) {
      struct window *fw = find_handle(focused_handle);
      if (fw)
        mark_dirty(fw->x, fw->y, outer_w(fw), outer_h(fw));
    }
  }

  mark_dirty(old_x, old_y, old_ow, old_oh);
  printf("winman: destroy handle=%d owner=%d active=%d\n", handle,
         old_owner_pid, window_count());
}

void handle_destroy(int client_pid, int handle) {
  handle_destroy_internal(handle, client_pid);
}

void destroy_windows_for_owner(int owner_pid, const char *why) {
  if (owner_pid <= 0)
    return;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use || windows[i].owner_pid != owner_pid)
      continue;
    int h = windows[i].handle;
    printf("winman: reap window %d owner_pid=%d (%s)\n", h, owner_pid,
           why ? why : "owner-exit");
    handle_destroy_internal(h, 0);
  }
}

/* True if window is currently minimized (and therefore must skip compose +
 * hit-test). Console can't be minimized. */
int is_minimized(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return 0;
  struct window *w = find_handle(handle);
  return w && w->minimized;
}

/* Toggle min/restore. While minimized the window stays on the taskbar and
 * keeps its z-order slot, but compose + hit-test skip it; the taskbar
 * button is the only way to bring it back. */
void toggle_minimize(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return;
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: toggle_minimize: no window found for handle=%d\n", handle);
    return;
  }
  w->minimized = !w->minimized;
  if (w->minimized && focused_handle == handle) {
    /* Refocus to next visible handle in z-order, preferring real
     * windows; falls back to console if nothing else qualifies. */
    focused_handle = 0;
    for (int i = 0; i < z_count; i++) {
      int h = z_order[i];
      if (h == handle)
        continue;
      if (is_minimized(h))
        continue;
      focused_handle = h;
      break;
    }
  }
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
}

/* Toggle maximize/restore. Saves pre-max geometry in window struct so the
 * second click restores. Uses client_window_resize so the client gets a
 * WM_RESIZE_NOTIFY with the new shared surface. */
void client_window_resize(int handle, int new_cw, int new_ch);
void win_set_pos(int handle, int x, int y);

void toggle_maximize(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return;
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: toggle_maximize: no window found for handle=%d\n", handle);
    return;
  }
  if (w->maximized) {
    client_window_resize(handle, w->saved_cw, w->saved_ch);
    win_set_pos(handle, w->saved_x, w->saved_y);
    w->maximized = 0;
  } else {
    w->saved_x = w->x;
    w->saved_y = w->y;
    w->saved_cw = w->client_w;
    w->saved_ch = w->client_h;
    win_set_pos(handle, 0, 0);
    client_window_resize(handle, max_client_w, max_client_h);
    w->maximized = 1;
  }
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
}

/* Walk the kernel proc table and reap any window whose owner is explicitly
 * terminal. Missing owner rows are ignored here: the kernel sends
 * IPC_PEER_EXITED from task_exit(), so absence in a snapshot is treated as
 * inconclusive rather than permission to destroy visible client state.
 *
 * Note: this does NOT unmap the shared pages from the (already-gone) owner's
 * address space. The kernel reclaims that pml4 when the task struct is
 * freed; the phys frames go back to PMM with it. */
void reap_dead_windows(void) {
  /* Increased to 256 so we don't miss processes if the system is busy */
  struct proc_info procs[256];
  long n = proc_list(procs, (long)(sizeof(procs) / sizeof(procs[0])));
  if (n < 0)
    return;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use)
      continue;
    int owner = windows[i].owner_pid;
    int terminal = 1; /* Assume dead if not found in the list */
    for (long j = 0; j < n; j++) {
      if (procs[j].pid == owner) {
        int s = procs[j].state;
        terminal = (s == PROC_STATE_ZOMBIE || s == PROC_STATE_DEAD);
        break;
      }
    }
    if (terminal) {
      int h = windows[i].handle;
      printf("winman: reap window %d owner_pid=%d (dead)\n", h, owner);
      handle_destroy_internal(h, 0);
    }
  }

  /* Same for consoles: a shell that ran off the end of `exit` leaves a window
   * nothing can type into. Unlike client windows, a missing row is treated as
   * dead here , the shell is winman's own child, so if it is not in the table
   * it is gone, and there is no IPC_PEER_EXITED for a process that never
   * registered as a WM client. */
  for (int i = 0; i < CON_MAX; i++) {
    struct console *c = &cons[i];
    if (!c->win.in_use || c->pid <= 0)
      continue;
    int terminal = 1;
    for (long j = 0; j < n; j++) {
      if (procs[j].pid == c->pid) {
        int s = procs[j].state;
        terminal = (s == PROC_STATE_ZOMBIE || s == PROC_STATE_DEAD);
        break;
      }
    }
    if (terminal) {
      printf("winman: reap console slot=%d sh pid=%d (exited)\n", i, c->pid);
      c->pid = 0; /* already gone , console_close has nothing to kill */
      console_close(c);
    }
  }
}

void handle_set_title(int owner_pid, int handle, const char *title) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid != owner_pid || !title)
    return;
  size_t i = 0;
  while (i < sizeof(w->title) - 1 && title[i]) {
    w->title[i] = title[i];
    i++;
  }
  w->title[i] = 0;
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
}
