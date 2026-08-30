/* kernel/display/print.c , legacy VGA text-mode writer.
 *
 * 80x25 character cell grid at 0xB8000 (the BIOS text-mode framebuffer).
 * Pre-dates the framebuffer/TTY stack; kept around because early boot
 * needs SOMETHING visible before the framebuffer is mapped. Once the TTY
 * is up, prefer that.
 */
#include <display/print.h>
#include <memory/hhdm.h>

const static usize NUM_COLS = 80;
const static usize NUM_ROWS = 25;

struct Char {
  u8 character;
  u8 color;
};

struct Char *buffer = (struct Char *)(HHDM_BASE + 0xb8000ULL);
usize col = 0;
usize row = 0;
u8 color = PRINT_COLOR_WHITE | PRINT_COLOR_BLACK << 4;

void clear_row(usize row) {
  struct Char empty = (struct Char){
    .character =  ' ',
    .color =  color,
  };

  for (usize col = 0; col < NUM_COLS; col++) {
    buffer[col + NUM_COLS * row] = empty;
  }
}

void print_clear() {
  for (usize i = 0; i < NUM_ROWS; i++) {
    clear_row(i);
  }
}

void print_newline() {
  col = 0;

  if (row < NUM_ROWS - 1) {
    row++;
    return;
  }

  for (usize row = 1; row < NUM_ROWS; row++) {
    for (usize col = 0; col < NUM_COLS; col++) {
      struct Char character = buffer[col + NUM_COLS * row];
      buffer[col + NUM_COLS * (row - 1)] = character;
    }
  }

  clear_row(NUM_COLS - 1);
}

void print_write_char(char character) {
  if (character == '\n') {
    print_newline();
    return;
  }

  if (col > NUM_COLS) {
    print_newline();
  }

  buffer[col + NUM_COLS * row] = (struct Char){
    .character =  (u8)character,
    .color =  color,
  };

  col++;
}

void print_write_str(const char *str) {
  for (usize i = 0; 1; i++) {
    char character = (u8)str[i];

    if (character == '\0') {
      return;
    }

    print_write_char(character);
  }
}

static void u64_to_hex(char *out, u64 value) {
    static const char digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'x';

    for (int i = 0; i < 16; i++) {
        int shift = 60 - (i * 4);
        out[2 + i] = digits[(value >> shift) & 0xF];
    }

    out[18] = '\0';
}

void print_write_hex(u64 hex) {
  char hex_str[19];
  u64_to_hex(hex_str, hex);
  print_write_str(hex_str);
}

void print_set_color(u8 foreground, u8 background) {
  color = foreground + (background << 4);
}
