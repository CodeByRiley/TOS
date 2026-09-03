/* Cross-module entry points for the Winman executable. This is a private
 * interface: applications use lib/wm.h and never include this file. */
#ifndef WINMAN_PROTOTYPES_H
#define WINMAN_PROTOTYPES_H

#include <stdint.h>
#include <stddef.h>
int cursor_w(void);
int cursor_h(void);
void update_client_size_limits(void);
void clamp_client_size(int *client_w, int *client_h);
void reset_titlebar_click(void);
int titlebar_click_is_double(int handle, int x, int y, uint32_t now);
void cursor_load(void);
void tb_load_icons(void);
void blend_px(uint32_t *dst, uint32_t src);
void desktop_load(void);
void copy_field(char *dst, size_t cap, const char *src);
int ascii_lower(int c);
int same_name_ci(const char *a, const char *b);
int start_menu_has_path(const char *path);
void start_menu_add(const char *name, const char *path);
int start_menu_capacity(void);
void start_menu_scan_dir(const char *dir, int capacity);
void build_start_menu_entries(void);
void z_remove(int handle);
void z_bring_to_front(int handle);
void z_send_to_back(int handle);
int outer_w(const struct window *w);
int outer_h(const struct window *w);
void build_con_glyph_lut(void);
void fill_dwords(uint32_t *dst, size_t n, uint32_t color);
void mark_dirty(int x, int y, int w, int h);
int con_cell_w_for_scale(int scale);
int con_cell_h_for_scale(int scale);
int con_cell_w(const struct console *c);
int con_cell_h(const struct console *c);
int con_font_px(const struct console *c);
int is_console_handle(int handle);
struct console *con_for_handle(int handle);
struct console *con_focused(void);
void con_try_load_ttf(void);
int win_get_rect(int handle, int *x, int *y, int *cw, int *ch);
void win_set_pos(int handle, int x, int y);
int outer_w_dims(int cw);
int outer_h_dims(int ch, int status_h);
int status_h_of(int handle);
int in_titlebar_btn(int win_x, int win_y, int outer_w,
                           int idx_from_right, int mx, int my);
int build_taskbar_entries(struct tb_entry *out, int max);
int taskbar_y(void);
int taskbar_btn_limit(void);
int hit_taskbar(int mx, int my, int *out_handle);
int in_start_menu(int menu_x, int menu_y, int mx, int my);
int hit_test_at(int mx, int my, int *out_handle);
void clamp_to_desktop(int *x, int *y, int cw, int ch);
void console_resize(struct console *c, int new_cw, int new_ch);
void client_window_resize(int handle, int new_cw, int new_ch);
int backbuffer_reserve(size_t required);
int backbuffer_register(void);
uint32_t *fb_pix(int x, int y);
void clip_set(int x, int y, int w, int h);
int clip_hits(int x, int y, int w, int h);
int clip_contains_point(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void draw_glyph_fb(int x, int y, char c, uint32_t fg, uint32_t bg);
void draw_text_fb(int x, int y, const char *s, int max_w, uint32_t fg,
                         uint32_t bg);
void titlebar_btn_rect(int win_x, int win_y, int outer_w,
                              int idx_from_right, int *bx, int *by, int *bw,
                              int *bh);
void draw_button_mask(int x, int y,
                             const uint8_t mask[TB_BTN_SIZE][TB_BTN_SIZE],
                             uint32_t fg, uint32_t bg);
void draw_chrome(const struct window *w, int focused);
void blit_surface(const struct window *w);
void blit_icon(int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
               int src_h, uint32_t *pixels);
void draw_start_button(int y);
void clock_format(void);
int clock_rect(int *cx, int *cy, int *cw, int *ch);
int network_rect(int *cx, int *cy, int *cw, int *ch);
int network_tick(void);
int clock_tick(void);
void draw_clock(void);
void draw_taskbar(void);
void draw_start_menu(void);
void con_draw_glyph(struct console *con, int gx, int gy, char c);
void con_redraw(struct console *con);
int con_set_scale(struct console *con, int new_scale);
void con_scroll(struct console *con);
void con_newline(struct console *con);
void con_wipe(struct console *con);
void con_save(struct console *con);
void con_restore(struct console *con);
void con_putc(struct console *con, char c);
void console_geometry(int slot, int *out_x, int *out_y, int *out_cw,
                             int *out_ch);
int con_alloc_buffers(struct console *con, int slot);
void con_set_title(struct console *con, int slot);
struct console *console_open(void);
void console_close(struct console *con);
int path_is_shell(const char *path);
void launch_program(const char *path);
void drain_tty_into_console(void);
void compose_handle(int handle);
void compose(void);
void present_backbuffer_rects(const struct fb_rect *rects,
                                     uint32_t rect_count);
void present_backbuffer_rect(int x, int y, int w, int h);
void present_full_desktop(void);
void present_dirty(void);
int cursor_scale(void);
int cursor_rect(int32_t x, int32_t y, int scale, struct fb_rect *rect);
void draw_cursor_with_repairs(int32_t x, int32_t y,
                                     const struct fb_rect *repairs,
                                     uint32_t repair_count);
void draw_cursor(int32_t x, int32_t y);
void present_cursor_repair_at(int32_t x, int32_t y);
void present_rect(int x, int y, int w, int h);
void erase_ghost(int x, int y, int w, int h);
void draw_ghost(int x, int y, int w, int h);
void compute_ghost(int mx, int my, int *gx, int *gy, int *gw, int *gh);
struct window *find_slot(void);
struct window *find_handle(int handle);
int handle_create(int client_pid, int w, int h, const char *title,
                         uint32_t flags, uint64_t *out_client_va,
                         uint32_t *out_pitch, int *out_handle);
int window_count(void);
void handle_destroy_internal(int handle, int client_pid_check);
void handle_destroy(int client_pid, int handle);
void destroy_windows_for_owner(int owner_pid, const char *why);
int is_minimized(int handle);
void toggle_minimize(int handle);
void toggle_maximize(int handle);
void reap_dead_windows(void);
void handle_set_title(int owner_pid, int handle, const char *title);
const char *prompt_button_label(int kind, int idx);
int prompt_button_count(int kind);
int prompt_button_result(int kind, int idx);
void prompt_freeze_rect(void);
void prompt_rect(int *px, int *py, int *pw, int *ph);
void prompt_btn_rect(int idx, int *bx, int *by, int *bw, int *bh);
void prompt_send_result(int result);
void handle_prompt_req(int owner_pid, int handle, int kind,
                              const char *message);
void prompt_abandon_for_owner(int owner_pid);
void draw_prompt(void);
int prompt_handle_key(int keycode, int shift);
int prompt_handle_click(int mx, int my);
void handle_set_status(int owner_pid, int handle, const char *text);
void send_create_resp(int target_pid, int handle, uint64_t va,
                             uint32_t pitch, int w, int h);
void pump_ipc(void);
void forward_input(int target_pid, int win_handle, const struct msg *m);
void request_window_close(int handle, uint32_t now);
void pump_input(void);
int main(int argc, char **argv);

#endif
