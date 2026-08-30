#define WINMAN_DECLARE_STATE
#include "key_codes.h"
#include "syscall.h"
#include "winman.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

int cursor_w(void) { return cursor_img.pixels ? cursor_img.width : CURSOR_W; }

int cursor_h(void) { return cursor_img.pixels ? cursor_img.height : CURSOR_H; }

/* Try the on-disk cursor once at startup. Failure is not an error worth
 * stopping for , it just leaves the built-in mask in place. */
void cursor_load(void) {
  if (bmp_load(CURSOR_BMP_PATH, &cursor_img) == 0) {
    if (cursor_img.width > 0 && cursor_img.height > 0 &&
        cursor_img.width <= CURSOR_MAX_SOURCE_DIM &&
        cursor_img.height <= CURSOR_MAX_SOURCE_DIM) {
      printf("winman: cursor %dx%d from %s\n", cursor_img.width,
             cursor_img.height, CURSOR_BMP_PATH);
    } else {
      printf(
          "winman: %s has invalid cursor dimensions, using built-in cursor\n",
          CURSOR_BMP_PATH);
      bmp_free(&cursor_img);
    }
  } else {
    printf("winman: %s unavailable, using built-in cursor\n", CURSOR_BMP_PATH);
  }
}

void desktop_load(void) {
  if (bmp_load("/system/wallpaper.bmp", &wallpaper_img) == 0) {
    wallpaper_loaded = 1;
    printf("winman: wallpaper %dx%d loaded\n", wallpaper_img.width,
           wallpaper_img.height);
  }

  /* Curated, unlike the start menu: each entry needs artwork at
   * /system/icons/<name>.bmp, so there is nothing to discover by scanning.
   * `name` is both the label and the artwork's basename; `path` must match
   * the destination in tools/create_disk.sh. */
  static const struct program desktop_candidates[] = {
      {"DOOM", "usr/bin/doom.elf"},
      {"shelf", "system/bin/sh.elf"},
  };
  const int candidate_count =
      (int)(sizeof(desktop_candidates) / sizeof(desktop_candidates[0]));

  desktop_icon_count = 0;
  for (int c = 0; c < candidate_count && desktop_icon_count < MAX_ICONS; c++) {
    /* Skip anything not actually on the volume. DOOM ships only when it was
     * built, so a hardcoded icon for it would otherwise sit on the desktop
     * doing nothing but fail to spawn when clicked. */
    struct stat_user st;
    if (stat_raw(desktop_candidates[c].path, &st) != 0) {
      printf("winman: desktop icon %s skipped, %s not present\n",
             desktop_candidates[c].name, desktop_candidates[c].path);
      continue;
    }

    struct desktop_icon *icon = &desktop_icons[desktop_icon_count++];
    icon->program = desktop_candidates[c];
    icon->x = 20;
    icon->y = 20 + (desktop_icon_count - 1) * 80;
    icon->w = 32;
    icon->h = 32;
  }

  for (int i = 0; i < desktop_icon_count; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/%s.bmp",
             desktop_icons[i].program.name);

    if (bmp_load(path, &desktop_icons[i].icon) == 0) {
      desktop_icons[i].loaded = 1;
    } else {
      printf("winman: icon %s not found, using fallback\n",
             desktop_icons[i].program.name);
      desktop_icons[i].loaded = 0;
    }
  }
}

/* Bounded strcpy into a fixed field; truncates rather than overflowing. */
void copy_field(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (cap == 0)
    return;
  while (src && src[i] && i + 1 < cap) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive compare , the volume is FAT, so DOOM.ELF and doom.elf are
 * the same file and must not both end up in the menu. */
int same_name_ci(const char *a, const char *b) {
  while (*a && *b) {
    if (ascii_lower(*a) != ascii_lower(*b))
      return 0;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

int start_menu_has_path(const char *path) {
  for (int i = 0; i < start_menu_count; i++) {
    if (same_name_ci(start_menu_programs[i].path, path))
      return 1;
  }
  return 0;
}

static const char *const skipped_programs[] = {
    "pkill", "plist",  "mtest",       "pe_test", "audiotest", "btop",
    "cat",   "ls",     "holyd",       "fdchild", "faulter",   "thread",
    "tree",  "stress", "stress_peer", "uidemo",  "vmtest",    "winman"};

static int is_skipped_program(const char *label) {
  for (size_t k = 0; k < sizeof(skipped_programs) / sizeof(skipped_programs[0]);
       k++) {
    if (same_name_ci(label, skipped_programs[k]))
      return 1;
  }
  return 0;
}

int start_menu_remove(const char *path) {
  for (int i = 0; i < start_menu_count; i++) {
    if (same_name_ci(start_menu_programs[i].path, path)) {
      memmove(&start_menu_programs[i], &start_menu_programs[i + 1],
              sizeof(start_menu_programs[0]) *
                  (size_t)(start_menu_count - i - 1));
      start_menu_count--;
      return 1;
    }
  }
  return 0;
}

void start_menu_add(const char *name, const char *path) {
  if (start_menu_count >= START_MENU_MAX)
    return;
  if (is_skipped_program(name)) {
    printf("winman: skip %s (%s)\n", name, path);
    start_menu_remove(path);
    return;
  }
  if (start_menu_has_path(path)) {
    printf("winman: dup %s (%s)\n", name, path);
    return;
  }
  printf("winman: add %s (%s)\n", name, path);
  struct start_entry *e = &start_menu_programs[start_menu_count++];
  copy_field(e->name, sizeof(e->name), name);
  copy_field(e->path, sizeof(e->path), path);
}

/* How many items fit between the top of the screen and the taskbar. The menu
 * is drawn upward from the taskbar, so without this an over-full directory
 * would place menu_y above 0 and the entries would be clipped off-screen
 * with no way to reach them. */
int start_menu_capacity(void) {
  int usable = fb_h - TASKBAR_PX - START_MENU_PAD * 2;
  int fits = usable > 0 ? usable / START_MENU_ITEM_H : 0;
  if (fits > START_MENU_MAX)
    fits = START_MENU_MAX;
  if (fits < START_MENU_DEFAULT_COUNT)
    fits = START_MENU_DEFAULT_COUNT; /* pinned entries always get a slot */
  return fits;
}

/* Append every *.elf in `dir` that is not already listed. */
void start_menu_scan_dir(const char *dir, int capacity) {
  char buf[512];
  unsigned index = 0;
  long n;

  while (start_menu_count < capacity &&
         (n = readdir_path(dir, &index, buf, sizeof(buf))) > 0) {
    /* Packed NUL-terminated names; directories come back with a trailing
     * '/' and are skipped. */
    for (long i = 0; i < n && start_menu_count < capacity; i++) {
      if (buf[i] == 0)
        continue;
      const char *entry = &buf[i];
      size_t len = strlen(entry);
      i += (long)len;

      if (len == 0 || entry[len - 1] == '/')
        continue;

      /* Only the ELF executables , the .exe PE variants are the same
       * programs under another name and would double up the menu. */
      if (len < 5 || !same_name_ci(entry + len - 4, ".elf"))
        continue;

      char path[START_MENU_PATH_MAX];
      copy_field(path, sizeof(path), dir);
      size_t at = strlen(path);
      copy_field(path + at, sizeof(path) - at, "/");
      at = strlen(path);
      copy_field(path + at, sizeof(path) - at, entry);

      /* Label is the filename without the extension. */
      char label[START_MENU_NAME_MAX];
      copy_field(label, sizeof(label), entry);
      size_t label_len = strlen(label);
      if (label_len > 4)
        label[label_len - 4] = 0;

      start_menu_add(label, path);
    }
  }
}

/* Pinned entries first, then each executable directory in order. A missing
 * directory just yields nothing , readdir_path returns <= 0 and the scan
 * moves on, so /usr/local/bin being empty or absent is not an error. */
void build_start_menu_entries(void) {
  start_menu_count = 0;
  for (int i = 0; i < START_MENU_DEFAULT_COUNT; i++)
    start_menu_add(start_menu_defaults[i].name, start_menu_defaults[i].path);

  int capacity = start_menu_capacity();
  for (int d = 0; d < START_MENU_SCAN_DIR_COUNT; d++)
    start_menu_scan_dir(start_menu_scan_dirs[d], capacity);

  printf("winman: start menu %d entries (%d pinned)\n", start_menu_count,
         START_MENU_DEFAULT_COUNT);
}

void z_remove(int handle) {
  int j = 0;
  for (int i = 0; i < z_count; i++) {
    if (z_order[i] == handle)
      continue;
    z_order[j++] = z_order[i];
  }
  z_count = j;
}

void z_bring_to_front(int handle) {
  z_remove(handle);
  if (z_count >= MAX_Z)
    return;
  for (int i = z_count; i > 0; i--)
    z_order[i] = z_order[i - 1];
  z_order[0] = handle;
  z_count++;
}

void z_send_to_back(int handle) {
  z_remove(handle);
  if (z_count >= MAX_Z)
    return;

  z_order[z_count] = handle;
  z_count++;
}
