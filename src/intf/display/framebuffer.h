#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

int      framebuffer_init(uint64_t mb2_addr);
void     framebuffer_clear(uint32_t color);
void     framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t *framebuffer_buffer(void);         // for DOOM/games to write directly

#endif
