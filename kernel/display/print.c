/* kernel/display/print.c — legacy VGA text-mode writer.
 *
 * 80x25 character cell grid at 0xB8000 (the BIOS text-mode framebuffer).
 * Pre-dates the framebuffer/TTY stack; kept around because early boot
 * needs SOMETHING visible before the framebuffer is mapped. Once the TTY
 * is up, prefer that.
 */
#include "display/print.h"
#include "memory/hhdm.h"

const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
  uint8_t character;
  uint8_t color;
};

struct Char *buffer = (struct Char *)(HHDM_BASE + 0xb8000ULL);
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | PRINT_COLOR_BLACK << 4;

void clear_row(size_t row) {
  struct Char empty = (struct Char){
    .character =  ' ',
    .color =  color,
  };

  for (size_t col = 0; col < NUM_COLS; col++) {
    buffer[col + NUM_COLS * row] = empty;
  }
}

void print_clear() {
  for (size_t i = 0; i < NUM_ROWS; i++) {
    clear_row(i);
  }
}

void print_newline() {
  col = 0;

  if (row < NUM_ROWS - 1) {
    row++;
    return;
  }

  for (size_t row = 1; row < NUM_ROWS; row++) {
    for (size_t col = 0; col < NUM_COLS; col++) {
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
    .character =  (uint8_t)character,
    .color =  color,
  };

  col++;
}

void print_write_str(const char *str) {
  for (size_t i = 0; 1; i++) {
    char character = (uint8_t)str[i];

    if (character == '\0') {
      return;
    }

    print_write_char(character);
  }
}

static void u64_to_hex(char *out, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'x';

    for (int i = 0; i < 16; i++) {
        int shift = 60 - (i * 4);
        out[2 + i] = digits[(value >> shift) & 0xF];
    }

    out[18] = '\0';
}

void print_write_hex(uint64_t hex) {
  char hex_str[19];
  u64_to_hex(hex_str, hex);
  print_write_str(hex_str);
}

void print_set_color(uint8_t foreground, uint8_t background) {
  color = foreground + (background << 4);
}
