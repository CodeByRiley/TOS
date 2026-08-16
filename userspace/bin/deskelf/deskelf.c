/* userspace/bin/deskelf/deskelf.c - DESKELF: Desktop File Explorer.
 *
 * Traverses FAT32/16 directory entries and manages file/folder operations.
 * Operates visually alongside the SH(ell).ELF prompt, listening for keyboard
 * events to navigate directory trees, launch executables via spawn(), and
 * organize the local volume.
 *
 */

#include <lib/gfx.h>
#include <lib/keymap.h>
#include <lib/syscall.h>
#include <lib/ui.h>
#include <lib/wm.h>
#include <include/key_codes.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_W 560
#define WINDOW_H 380
#define MIN_LAYOUT_W 300
#define MIN_LAYOUT_H 180

#define MAX_ENTRIES 128
#define MAX_NAME 256
#define MAX_PATH 256
#define STATUS_CAP 128
#define TEXT_INPUT_CAP MAX_NAME

#define MODAL_NONE 0
#define MODAL_MKDIR 1
#define MODAL_DELETE 2

#define UI_ID_UP 1
#define UI_ID_OPEN 2
#define UI_ID_MKDIR 3
#define UI_ID_DELETE 4
#define UI_ID_QUIT 5
#define UI_ID_CONFIRM 6
#define UI_ID_CANCEL 7
#define UI_ID_ENTRY_BASE 100

struct file_entry {
    char name[MAX_NAME];
    int is_dir;
};

static char cwd[MAX_PATH];
static struct file_entry entries[MAX_ENTRIES];
static int entry_count;
static int selected;
static int scroll;

static int modal_action;
static int modal_target_is_dir;
static char text_input[TEXT_INPUT_CAP];
static int text_input_len;
static char status_text[STATUS_CAP];
static int left_shift_held;
static int right_shift_held;

static size_t bounded_strlen(const char *text, size_t max) {
    size_t length = 0;
    if (!text)
        return 0;
    while (length < max && text[length])
        length++;
    return length;
}

static int copy_string(char *dst, size_t capacity, const char *src) {
    if (!dst || capacity == 0 || !src)
        return -1;
    size_t length = bounded_strlen(src, capacity);
    if (length >= capacity)
        return -1;
    memcpy(dst, src, length + 1);
    return 0;
}

static void set_status(const char *message) {
    if (copy_string(status_text, sizeof(status_text), message) != 0)
        status_text[0] = 0;
}

static int join_path(char *dst, size_t capacity, const char *dir,
                     const char *name) {
    if (!dst || capacity == 0 || !dir || !name)
        return -1;

    size_t dir_len = bounded_strlen(dir, capacity);
    size_t name_len = bounded_strlen(name, MAX_NAME);
    if (dir_len >= capacity || name_len >= MAX_NAME)
        return -1;

    int needs_slash = dir_len == 0 || dir[dir_len - 1] != '/';
    size_t required = dir_len + (size_t)needs_slash + name_len + 1;
    if (required > capacity)
        return -1;

    memcpy(dst, dir, dir_len);
    size_t offset = dir_len;
    if (needs_slash)
        dst[offset++] = '/';
    memcpy(dst + offset, name, name_len);
    dst[offset + name_len] = 0;
    return 0;
}

static void clamp_selection(void) {
    if (entry_count <= 0) {
        selected = 0;
        scroll = 0;
        return;
    }
    if (selected < 0)
        selected = 0;
    if (selected >= entry_count)
        selected = entry_count - 1;
    if (scroll < 0)
        scroll = 0;
    if (scroll > selected)
        scroll = selected;
}

static int load_entries(void) {
    entry_count = 0;

    copy_string(entries[entry_count].name, MAX_NAME, "..");
    entries[entry_count].is_dir = 1;
    entry_count++;

    unsigned index = 0;
    char buffer[1024];
    long result = 0;

    while (entry_count < MAX_ENTRIES) {
        result = readdir_path(cwd, &index, buffer, sizeof(buffer));
        if (result <= 0)
            break;

        long offset = 0;
        while (offset < result && entry_count < MAX_ENTRIES) {
            const char *name = buffer + offset;
            size_t remaining = (size_t)(result - offset);
            size_t length = bounded_strlen(name, remaining);
            if (length == remaining) {
                set_status("Directory returned a malformed entry");
                clamp_selection();
                return -1;
            }
            offset += (long)length + 1;
            if (length == 0)
                continue;

            int directory = name[length - 1] == '/';
            size_t display_len = directory ? length - 1 : length;
            if (display_len == 0 || display_len >= MAX_NAME)
                continue;
            if (display_len == 1 && name[0] == '.')
                continue;
            if (display_len == 2 && name[0] == '.' && name[1] == '.')
                continue;

            memcpy(entries[entry_count].name, name, display_len);
            entries[entry_count].name[display_len] = 0;
            entries[entry_count].is_dir = directory;
            entry_count++;
        }
    }

    clamp_selection();
    if (result < 0) {
        set_status("Could not read this directory");
        return -1;
    }
    if (entry_count == MAX_ENTRIES)
        set_status("Directory list truncated");
    return 0;
}

static void go_up(void) {
    size_t length = bounded_strlen(cwd, sizeof(cwd));
    if (length <= 1)
        return;

    while (length > 1 && cwd[length - 1] == '/')
        cwd[--length] = 0;
    while (length > 1 && cwd[length - 1] != '/')
        length--;

    if (length <= 1) {
        cwd[0] = '/';
        cwd[1] = 0;
    } else {
        cwd[length - 1] = 0;
    }
    selected = 0;
    scroll = 0;
    load_entries();
}

static int selected_valid(void) {
    return selected >= 0 && selected < entry_count;
}

static void enter_selected_directory(void) {
    if (!selected_valid() || !entries[selected].is_dir)
        return;
    if (selected == 0) {
        go_up();
        return;
    }

    char path[MAX_PATH];
    if (join_path(path, sizeof(path), cwd, entries[selected].name) != 0 ||
        copy_string(cwd, sizeof(cwd), path) != 0) {
        set_status("Path is too long");
        return;
    }
    selected = 0;
    scroll = 0;
    load_entries();
}

static void execute_selected(void) {
    if (!selected_valid() || selected == 0 || entries[selected].is_dir)
        return;

    char path[MAX_PATH];
    if (join_path(path, sizeof(path), cwd, entries[selected].name) != 0) {
        set_status("Path is too long");
        return;
    }

    char *argv[] = {entries[selected].name, 0};
    if (spawn(path, argv) < 0)
        set_status("Could not launch the selected file");
    else
        set_status("Program launch queued");
}

static void activate_selected(void) {
    if (!selected_valid())
        return;
    if (entries[selected].is_dir)
        enter_selected_directory();
    else
        execute_selected();
}

static void open_mkdir_modal(void) {
    modal_action = MODAL_MKDIR;
    text_input[0] = 0;
    text_input_len = 0;
}

static void open_delete_modal(void) {
    if (!selected_valid() || selected == 0)
        return;
    if (copy_string(text_input, sizeof(text_input), entries[selected].name) !=
        0) {
        set_status("Name is too long");
        return;
    }
    text_input_len = (int)bounded_strlen(text_input, sizeof(text_input));
    modal_target_is_dir = entries[selected].is_dir;
    modal_action = MODAL_DELETE;
}

static void cancel_modal(void) {
    modal_action = MODAL_NONE;
    text_input[0] = 0;
    text_input_len = 0;
}

static void confirm_modal(void) {
    if (modal_action == MODAL_NONE)
        return;
    text_input[text_input_len] = 0;
    if (text_input_len == 0) {
        set_status("A name is required");
        return;
    }

    char path[MAX_PATH];
    if (join_path(path, sizeof(path), cwd, text_input) != 0) {
        set_status("Path is too long");
        return;
    }

    long result;
    if (modal_action == MODAL_MKDIR) {
        result = mkdir_path(path);
        set_status(result == 0 ? "Directory created" :
                                 "Could not create directory");
    } else {
        result = modal_target_is_dir ? rmdir_path(path) : unlink(path);
        if (result == 0)
            set_status(modal_target_is_dir ? "Directory deleted" :
                                             "File deleted");
        else
            set_status(modal_target_is_dir ?
                           "Directory is not empty or could not be deleted" :
                           "Could not delete file");
    }

    if (result == 0) {
        modal_action = MODAL_NONE;
        load_entries();
    }
}

static int handle_key(int key, int pressed, int *running) {
    if (key == KEY_LEFTSHIFT) {
        left_shift_held = pressed;
        return 1;
    }
    if (key == KEY_RIGHTSHIFT) {
        right_shift_held = pressed;
        return 1;
    }
    if (!pressed)
        return 0;

    if (modal_action != MODAL_NONE) {
        if (key == KEY_ESC) {
            cancel_modal();
            return 1;
        }
        if (key == KEY_ENTER || key == KEY_KPENTER) {
            confirm_modal();
            return 1;
        }
        if (modal_action == MODAL_DELETE)
            return 0;
        if (key == KEY_BACKSPACE) {
            if (text_input_len > 0)
                text_input[--text_input_len] = 0;
            return 1;
        }

        char character = keymap_to_ascii(
            (uint16_t)key, left_shift_held || right_shift_held);
        if (character >= 32 && character <= 126 && character != '/' &&
            character != '\\' && text_input_len + 1 < TEXT_INPUT_CAP) {
            text_input[text_input_len++] = character;
            text_input[text_input_len] = 0;
            return 1;
        }
        return 0;
    }

    if (key == KEY_ESC) {
        *running = 0;
        return 1;
    }
    if (key == KEY_UP) {
        if (selected > 0)
            selected--;
        return 1;
    }
    if (key == KEY_DOWN) {
        if (selected + 1 < entry_count)
            selected++;
        return 1;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        activate_selected();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_LEFT) {
        go_up();
        return 1;
    }

    char character = keymap_to_ascii(
        (uint16_t)key, left_shift_held || right_shift_held);
    if (character == 'q' || character == 'Q') {
        *running = 0;
        return 1;
    }
    if (character == 'k' || character == 'w') {
        if (selected > 0)
            selected--;
        return 1;
    }
    if (character == 'j' || character == 's') {
        if (selected + 1 < entry_count)
            selected++;
        return 1;
    }
    if (character == 'h') {
        go_up();
        return 1;
    }
    return 0;
}

static int draw_modal(struct ui_context *ui, struct gfx_surface *surface) {
    int width = surface->w - 20;
    if (width > 320)
        width = 320;
    if (width < 120)
        width = 120;
    int height = 104;
    int x = (surface->w - width) / 2;
    int y = (surface->h - height) / 2;
    struct gfx_rect modal = gfx_rect_make(x, y, width, height);
    ui_panel(ui, modal);

    const char *title = modal_action == MODAL_MKDIR ?
                            "New directory" :
                            (modal_target_is_dir ? "Delete directory" :
                                                   "Delete file");
    ui_label(ui, gfx_rect_make(x + 10, y + 6, width - 20, 18), title);

    struct gfx_rect input = gfx_rect_make(x + 10, y + 28, width - 20, 22);
    ui_well(ui, input);
    if (modal_action == MODAL_MKDIR) {
        char display[TEXT_INPUT_CAP + 1];
        size_t length = bounded_strlen(text_input, sizeof(text_input));
        memcpy(display, text_input, length);
        display[length++] = '_';
        display[length] = 0;
        ui_label(ui, gfx_rect_inset(input, 2), display);
    } else {
        ui_label(ui, gfx_rect_inset(input, 2), text_input);
    }

    struct gfx_rect buttons = gfx_rect_make(x + 10, y + 58, width - 20, 22);
    int changed = 0;
    if (ui_button_id(ui, UI_ID_CONFIRM,
                     ui_layout_column(buttons, 2, 0, 8), "Confirm")) {
        confirm_modal();
        changed = 1;
    }
    if (ui_button_id(ui, UI_ID_CANCEL,
                     ui_layout_column(buttons, 2, 1, 8), "Cancel")) {
        cancel_modal();
        changed = 1;
    }
    ui_label_muted(ui, gfx_rect_make(x + 10, y + 84, width - 20, 14),
                   "Enter confirms; Esc cancels");
    return changed;
}

static int draw_frame(struct ui_context *ui, struct gfx_surface *surface,
                      int *running) {
    int changed = 0;
    int modal_down = ui->down;
    int modal_was_down = ui->was_down;
    if (modal_action != MODAL_NONE) {
        ui->down = 0;
        ui->was_down = 0;
    }
    struct gfx_rect bounds = gfx_surface_bounds(surface);
    gfx_fill(surface, bounds, ui->theme->face);

    if (surface->w < MIN_LAYOUT_W || surface->h < MIN_LAYOUT_H) {
        ui_label_centered(ui, bounds, "Window too small");
        return 0;
    }

    struct gfx_rect top = gfx_rect_make(0, 0, surface->w, 30);
    ui_panel(ui, top);
    ui_label(ui, gfx_rect_make(10, 5, surface->w - 100, 20), cwd);
    if (ui_button_id(ui, UI_ID_UP,
                     gfx_rect_make(surface->w - 80, 5, 70, 20), "Up")) {
        go_up();
        changed = 1;
    }

    const int bottom_h = 30;
    const int status_h = 18;
    struct gfx_rect bottom =
        gfx_rect_make(0, surface->h - bottom_h, surface->w, bottom_h);
    ui_panel(ui, bottom);
    struct gfx_rect commands = gfx_rect_inset(bottom, 5);
    if (ui_button_id(ui, UI_ID_OPEN,
                     ui_layout_column(commands, 4, 0, 5), "Open")) {
        activate_selected();
        changed = 1;
    }
    if (ui_button_id(ui, UI_ID_MKDIR,
                     ui_layout_column(commands, 4, 1, 5), "New")) {
        open_mkdir_modal();
        changed = 1;
    }
    if (ui_button_id(ui, UI_ID_DELETE,
                     ui_layout_column(commands, 4, 2, 5), "Delete")) {
        open_delete_modal();
        changed = 1;
    }
    if (ui_button_id(ui, UI_ID_QUIT,
                     ui_layout_column(commands, 4, 3, 5), "Quit")) {
        *running = 0;
        changed = 1;
    }

    struct gfx_rect status =
        gfx_rect_make(6, surface->h - bottom_h - status_h, surface->w - 12,
                      status_h);
    ui_label_muted(ui, status, status_text);

    int list_y = top.h + 5;
    int list_h = surface->h - top.h - bottom_h - status_h - 10;
    struct gfx_rect list = gfx_rect_make(5, list_y, surface->w - 10, list_h);
    ui_panel(ui, list);

    const int row_h = 18;
    int visible = (list.h - 8) / row_h;
    if (visible < 1)
        visible = 1;
    if (selected < scroll)
        scroll = selected;
    if (selected >= scroll + visible)
        scroll = selected - visible + 1;
    if (scroll < 0)
        scroll = 0;

    for (int row = 0; row < visible && scroll + row < entry_count; row++) {
        int index = scroll + row;
        struct gfx_rect rect =
            gfx_rect_make(list.x + 12, list.y + 4 + row * row_h,
                          list.w - 16, row_h - 2);
        char label[MAX_NAME + 8];
        label[0] = '[';
        label[1] = entries[index].is_dir ? 'D' : 'F';
        label[2] = ']';
        label[3] = ' ';
        copy_string(label + 4, sizeof(label) - 4, entries[index].name);

        if (ui_button_id(ui, UI_ID_ENTRY_BASE + index, rect, label)) {
            selected = index;
            changed = 1;
        }
        if (index == selected) {
            struct gfx_rect marker =
                gfx_rect_make(rect.x - 6, rect.y + 2, 4, rect.h - 4);
            gfx_fill(surface, marker, ui->theme->accent);
        }
    }

    if (modal_action != MODAL_NONE) {
        ui->down = modal_down;
        ui->was_down = modal_was_down;
        changed |= draw_modal(ui, surface);
    }
    return changed;
}

int main(void) {
    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] == 0)
        copy_string(cwd, sizeof(cwd), "/");
    set_status("Ready");
    load_entries();

    struct wm_window window;
    if (wm_window_create(WINDOW_W, WINDOW_H, "Desk Elf", &window) != 0) {
        printf("deskelf: could not create window\n");
        return 1;
    }

    struct gfx_surface surface;
    gfx_surface_init(&surface, (uint32_t *)(uintptr_t)window.surface_va,
                     window.w, window.h, (int)(window.pitch / 4));
    printf("deskelf: ready handle=%d cwd=%s\n", window.handle, cwd);
    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));

    int mouse_x = 0;
    int mouse_y = 0;
    int buttons = 0;
    int running = 1;
    int redraw = 1;

    while (running) {
        int saw_event = 0;
        int button_edges = 0;
        struct wm_event event;
        while (button_edges < 1 && wm_poll_event(&event)) {
            saw_event = 1;
            switch (event.type) {
            case WM_EV_KEY_DOWN:
                redraw |= handle_key(event.param, 1, &running);
                break;
            case WM_EV_KEY_UP:
                redraw |= handle_key(event.param, 0, &running);
                break;
            case WM_EV_MOUSE_MOVE:
                mouse_x = event.x;
                mouse_y = event.y;
                redraw = 1;
                break;
            case WM_EV_MOUSE_DOWN:
                mouse_x = event.x;
                mouse_y = event.y;
                buttons |= event.param;
                button_edges++;
                redraw = 1;
                break;
            case WM_EV_MOUSE_UP:
                mouse_x = event.x;
                mouse_y = event.y;
                buttons &= ~event.param;
                button_edges++;
                redraw = 1;
                break;
            case WM_EV_RESIZE:
                window.surface_va = event.surface_va;
                window.pitch = event.pitch;
                window.w = event.w;
                window.h = event.h;
                gfx_surface_init(&surface,
                                 (uint32_t *)(uintptr_t)window.surface_va,
                                 window.w, window.h,
                                 (int)(window.pitch / 4));
                memset(&ui, 0, sizeof(ui));
                redraw = 1;
                break;
            case WM_EV_QUIT:
                running = 0;
                break;
            default:
                break;
            }
        }

        if (!running)
            break;
        if (!redraw && !saw_event) {
            sleep_ticks(1);
            continue;
        }

        ui_begin(&ui, &surface, &ui_theme_default, mouse_x, mouse_y, buttons);
        int changed = draw_frame(&ui, &surface, &running);
        ui_end(&ui);
        wm_window_invalidate(window.handle);
        redraw = changed;
        yield();
    }

    wm_window_destroy(window.handle);
    printf("deskelf: exit\n");
    return 0;
}
