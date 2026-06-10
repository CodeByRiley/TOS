/* src/intf/display/print.h — VGA text-mode print helpers.
 *
 * Legacy 80x25 text-mode writers. Predate the framebuffer/TTY stack and
 * are kept around because early-boot code runs before the framebuffer is
 * mapped. Once the TTY comes up, prefer that.
 *
 * Implementation: src/impl/kernel/display/print.c.
 */
#ifndef PRINT_H
#define PRINT_H

#include <stddef.h>
#include <stdint.h>

/* Standard CGA palette. */
enum {
    PRINT_COLOR_BLACK       = 0,
    PRINT_COLOR_BLUE        = 1,
    PRINT_COLOR_GREEN       = 2,
    PRINT_COLOR_CYAN        = 3,
    PRINT_COLOR_RED         = 4,
    PRINT_COLOR_MAGENTA     = 5,
    PRINT_COLOR_BROWN       = 6,
    PRINT_COLOR_LIGHT_GRAY  = 7,
    PRINT_COLOR_DARK_GRAY   = 8,
    PRINT_COLOR_LIGHT_BLUE  = 9,
    PRINT_COLOR_LIGHT_GREEN = 10,
    PRINT_COLOR_LIGHT_CYAN  = 11,
    PRINT_COLOR_LIGHT_RED   = 12,
    PRINT_COLOR_PINK        = 13,
    PRINT_COLOR_YELLOW      = 14,
    PRINT_COLOR_WHITE       = 15,
};

void print_clear(void);
void print_newline(void);
void print_write_char(char character);
void print_write_str(const char *string);
void print_write_hex(uint64_t hex);
void print_set_color(uint8_t foreground, uint8_t background);

#endif
