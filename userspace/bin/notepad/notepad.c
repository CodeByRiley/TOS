#include <include/key_codes.h>
#include <lib/gfx.h>
#include <lib/keymap.h>
#include <lib/syscall.h>
#include <lib/ttf.h>
#include <lib/wm.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 480
#define WIN_H 320
#define MAX_CHARS 8192
#define FONT_PX 16
#define PATH_MAX 260

// Colors
#define COLOR_BG 0x00FFFFFF // White
#define COLOR_FG 0x00000000 // Black

// Globals
static uint32_t *surface = 0;
static int surf_w = 0;
static int surf_h = 0;
static int win_handle = -1;
static int shift_pressed = 0;
static int alt_pressed = 0;
static int ctrl_pressed = 0;

static int font_ready = 0;
static int cell_w = 9;
static int cell_h = 18;
static int ascent = 12;
static int descent = -3;

static char text_buf[MAX_CHARS];
static int text_len = 0;

/* The path is kept rather than an open FILE*: saving reopens with "w" every
 * time, which is what truncates. Holding one handle open across saves would
 * either append a second copy of the text or leave the tail of a longer
 * previous version behind, and could only ever be closed once. */
static char file_path[PATH_MAX];
static int have_file = 0;
static int dirty = 0;

static void clear_surface(void) {
  if (!surface)
    return;
  // Fast 32-bit fill for white background
  uint32_t *p = surface;
  for (int i = 0; i < surf_w * surf_h; i++) {
    p[i] = COLOR_BG;
  }
}

static const char *file_name(void) {
  if (!have_file)
    return "(untitled)";
  const char *slash = strrchr(file_path, '/');
  return slash ? slash + 1 : file_path;
}

/* Point the chrome title at the current file, with a leading '*' while there
 * are unsaved edits. */
static void update_title(void) {
  if (win_handle < 0)
    return;

  char title[64];
  snprintf(title, sizeof(title), "%sNotepad - %s", dirty ? "*" : "",
           file_name());
  wm_window_set_title(win_handle, title);
}

/* Status line: what file, how big, and whether it needs saving. Replaced
 * wholesale by transient messages from save/load, which the next edit or
 * save overwrites. */
static void update_status(void) {
  if (win_handle < 0)
    return;

  char line[48];
  snprintf(line, sizeof(line), "%s  %d bytes%s  Ctrl+S save", file_name(),
           text_len, dirty ? "  (modified)" : "");
  wm_window_set_status(win_handle, line);
}

static void set_status(const char *msg) {
  if (win_handle >= 0)
    wm_window_set_status(win_handle, msg);
}

/* First edit since the last save flips the title to '*'. Gated so ordinary
 * typing doesn't spend an IPC round trip per keystroke. */
static void mark_modified(void) {
  if (dirty)
    return;
  dirty = 1;
  update_title();
  update_status();
}

/* Read `path` into text_buf. A missing file is not an error — it just means
 * this is a new document that save will create. */
static void load_text_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("notepad: %s not found, starting empty\n", path);
    return;
  }

  /* One bulk read rather than a getc loop: fgetc lives in lib/stdio_extra.o,
   * which only the DOOM build links, and reading byte-at-a-time through an
   * unbuffered FILE is a syscall per character regardless. The MAX_CHARS
   * cap is the buffer bound — a longer file loads its head and says so. */
  size_t got = fread(text_buf, 1, MAX_CHARS, f);
  fclose(f);

  int count = (int)got;
  text_len = count;
  if (count == MAX_CHARS)
    printf("notepad: %s truncated to %d bytes\n", path, MAX_CHARS);
  else
    printf("notepad: loaded %d bytes from %s\n", count, path);
}

/* Ask winman for a path. Returns 1 if file_path now holds one. */
static int prompt_for_path(void) {
  char entered[PATH_MAX];
  int rc = wm_prompt(win_handle, WM_PROMPT_TEXT,
                     "Save as (full path):", entered, sizeof(entered));
  if (rc != WM_PROMPT_OK || !entered[0])
    return 0;

  strncpy(file_path, entered, sizeof(file_path) - 1);
  file_path[sizeof(file_path) - 1] = 0;
  have_file = 1;
  return 1;
}


/* save_as: always ask for a path (Ctrl+Shift+S). Otherwise write straight
 * back to the open file, asking only when the buffer has never had a name.
 * A cancelled prompt leaves any existing path untouched, so a cancelled
 * Save As does not detach the document from its file. */
static void save_text(int save_as) {
  if ((save_as || !have_file) && !prompt_for_path()) {
    set_status("Save cancelled");
    return;
  }

  /* "w" maps to O_WRONLY|O_CREAT|O_TRUNC, so the old contents go away
   * before the new ones land. */
  FILE *f = fopen(file_path, "w");
  if (!f) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Cannot write %s", file_name());
    wm_prompt(win_handle, WM_PROMPT_MESSAGE, msg, 0, 0);
    set_status("Save failed");
    printf("notepad: cannot open %s for writing\n", file_path);
    return;
  }

  size_t written = fwrite(text_buf, 1, (size_t)text_len, f);
  fclose(f);

  if (written != (size_t)text_len) {
    wm_prompt(win_handle, WM_PROMPT_MESSAGE, "Short write — disk full?", 0, 0);
    set_status("Save failed");
    printf("notepad: short write to %s (%d of %d bytes)\n", file_path,
           (int)written, text_len);
    return;
  }

  dirty = 0;
  update_title();

  char msg[48];
  snprintf(msg, sizeof(msg), "Saved %d bytes to %s", text_len, file_name());
  set_status(msg);
  printf("notepad: saved %d bytes to %s\n", text_len, file_path);
}

/* Yes/No/Cancel gate in front of anything that throws the buffer away.
 * Returns 1 when the caller may proceed. Shared by close and new so the two
 * cannot answer the question differently.
 *
 * Yes saves first and only proceeds if that worked — a save that failed or
 * whose Save As prompt was cancelled must not then discard the text it was
 * trying to preserve. */
static int confirm_discard(const char *question) {
  if (!dirty)
    return 1;

  int rc = wm_prompt(win_handle, WM_PROMPT_CONFIRM, question, 0, 0);
  if (rc == WM_PROMPT_CANCEL)
    return 0;
  if (rc == WM_PROMPT_OK) {
    save_text(0);
    return !dirty;
  }
  return 1; /* No — discard the edits */
}

/* Ctrl+N. Empties the buffer and detaches it from any file, so the next
 * save asks where to put it. */
static void new_file(void) {
  if (!confirm_discard("Save changes before the new file?"))
    return;

  text_len = 0;
  text_buf[0] = 0;
  file_path[0] = 0;
  have_file = 0;
  dirty = 0;
  update_title();
  update_status();
  printf("notepad: new file\n");
}

static void render_text(void) {
  if (!surface)
    return;
  clear_surface();

  if (!font_ready)
    return;

  struct gfx_surface s;
  gfx_surface_init(&s, surface, surf_w, surf_h, surf_w);

  int x = 0;
  int y = 0;
  int baseline = y + ascent;

  for (int i = 0; i < text_len; i++) {
    char c = text_buf[i];

    if (c == '\n') {
      x = 0;
      y += cell_h;
      baseline = y + ascent;
      continue;
    }

    // Word wrap
    if (x + cell_w > surf_w) {
      x = 0;
      y += cell_h;
      baseline = y + ascent;
    }

    // Out of bounds vertical scroll (simple cut-off for now)
    if (y + cell_h > surf_h) {
      break;
    }

    if (c >= 32 && c <= 126) {
      // Set a clip rect so glyphs don't bleed over the edge
      struct gfx_rect prev =
          gfx_clip_push(&s, gfx_rect_make(x, y, cell_w, cell_h));
      ttf_draw_glyph_cell(&s, g_sys_font, x, baseline, cell_w, (unsigned char)c,
                          FONT_PX, COLOR_FG);
      gfx_clip_set(&s, prev);
    }

    x += cell_w;
  }
}

static void invalidate(void) { wm_window_invalidate(win_handle); }

int main(int argc, char **argv) {
  /* `notepad <path>` opens that file; with no argument the buffer starts
   * empty and Ctrl+S has nowhere to write. */
  if (argc > 1 && argv[1] && argv[1][0]) {
    strncpy(file_path, argv[1], sizeof(file_path) - 1);
    file_path[sizeof(file_path) - 1] = 0;
    have_file = 1;
  }

  /* g_sys_font starts NULL in every process — nothing in crt0 or libwm
   * populates it, so an app wanting the shared font has to ask for it. */
  ttf_init_font();
  if (g_sys_font) {
    cell_w = ttf_cell_width(g_sys_font, FONT_PX);
    if (cell_w < 4)
      cell_w = 8;
    if (cell_w > 20)
      cell_w = 20;
    cell_h = FONT_PX + 3;
    ttf_vmetrics(g_sys_font, FONT_PX, &ascent, &descent, 0);
    if (ascent <= 0)
      ascent = (FONT_PX * 3) / 4;
    font_ready = 1;
    printf("notepad: TTF loaded cell=%dx%d\n", cell_w, cell_h);
  } else {
    printf("notepad: TTF failed to load!\n");
    return 1;
  }

  /* Window creation goes through libwm rather than a hand-rolled handshake:
   * it addresses winman by its real pid (wm_pid()) and spins on the reply
   * with a timeout instead of reading the queue once. */
  struct wm_window win;
  if (wm_window_create_ex(WIN_W, WIN_H, "Notepad", WM_CREATE_STATUSBAR, &win) !=
      0) {
    printf("notepad: Failed to get window\n");
    return 1;
  }

  win_handle = win.handle;
  surf_w = win.w;
  surf_h = win.h;
  surface = (uint32_t *)win.surface_va;
  if (win_handle < 0 || !surface) {
    printf("notepad: Failed to get window\n");
    return 1;
  }

  /* Load after the window exists so a truncation notice has somewhere to go
   * and the title can name the file. */
  if (have_file)
    load_text_file(file_path);
  update_title();
  update_status();

  // Initial Render
  render_text();
  invalidate();

  // Event Loop
  struct wm_event event;
  int running = 1;
  while (running) {
    /* wm_poll_event returns 1 when it filled `event`, 0 when the queue was
     * empty — the raw ipc_recv this used to call has the same polarity. */
    if (wm_poll_event(&event)) {
      /* The titlebar close button is a request. Winman sends WM_EV_QUIT and
       * waits for the owner to tear its own window down. An app that ignores it
       * simply never closes. */
      if (event.type == WM_EV_QUIT) {
        /* Unsaved work gets a say before the window goes. Cancel keeps the
         * app alive; winman treats a second close click as "force" anyway,
         * so an indecisive user is never trapped. */
        if (!confirm_discard("Save changes before closing?"))
          continue;
        running = 0;
        break;
      }
      /* Message loop */
      {
        int msg_type = event.type;
        int keycode = event.param;

        if (msg_type == WM_EV_KEY_DOWN) {
          int needs_redraw = 0;

          /* One chain, so a modifier keypress can't also fall through into
           * an editing case, and Ctrl+<key> is always a command rather than
           * text: Ctrl+Enter used to insert a newline. */
          if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
            shift_pressed = 1;
          } else if (keycode == KEY_LEFTALT || keycode == KEY_RIGHTALT) {
            alt_pressed = 1;
          } else if (keycode == KEY_LEFTCTRL || keycode == KEY_RIGHTCTRL) {
            ctrl_pressed = 1;
          }
          // Commands
          else if (ctrl_pressed) {
            if (shift_pressed) {
              if (keycode == KEY_S) {
                save_text(1);
              }
            } else {
              if (keycode == KEY_S) {
                save_text(0);
              } else if (keycode == KEY_O) {
                /* Reopen == reload. There is no file picker yet, so this
                 * rereads the file already open and drops unsaved edits
                 * rather than inventing a filename to open. */
                if (have_file) {
                  text_len = 0;
                  load_text_file(file_path);
                  dirty = 0;
                  update_title();
                  update_status();
                  needs_redraw = 1;
                } else {
                  printf("notepad: nothing to reload — launch as "
                         "'notepad <path>'\n");
                }
              } else if (keycode == KEY_N) {
                new_file();
                needs_redraw = 1;
              }
            }
          }
          // Enter
          else if (keycode == KEY_ENTER || keycode == KEY_KPENTER) {
            if (text_len < MAX_CHARS - 1) {
              text_buf[text_len++] = '\n';
              mark_modified();
              needs_redraw = 1;
            }
          }
          // Backspace
          else if (keycode == KEY_BACKSPACE) {
            if (text_len > 0) {
              text_len--;
              mark_modified();
              needs_redraw = 1;
            }
          }
          // Tab
          else if (keycode == KEY_TAB) {
            if (text_len < MAX_CHARS - 1) {
              text_buf[text_len++] = '\t';
              mark_modified();
              needs_redraw = 1;
            }
          }
          // Printable chars via your civilized keymap
          else {
            char c = keymap_to_ascii(keycode, shift_pressed);

            if (c >= 32 && c <= 126 && text_len < MAX_CHARS - 1) {
              text_buf[text_len++] = c;
              mark_modified();
              needs_redraw = 1;
            }
          }

          if (needs_redraw) {
            render_text();
            invalidate();
          }
        }

        // Track key releases for Shift
        else if (msg_type == WM_EV_KEY_UP) {
          if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
            shift_pressed = 0;
          } else if (keycode == KEY_LEFTALT || keycode == KEY_RIGHTALT) {
            alt_pressed = 0;
          } else if (keycode == KEY_LEFTCTRL || keycode == KEY_RIGHTCTRL) {
            ctrl_pressed = 0;
          }
        }
      }
      if (event.type == WM_EV_RESIZE) {
        surf_w = event.w;
        surf_h = event.h;
        surface = (uint32_t *)event.surface_va;
        render_text();
        invalidate();
      }
    }
    yield();
  }

  wm_window_destroy(win_handle);
  printf("notepad: exit\n");
  return 0;
}
