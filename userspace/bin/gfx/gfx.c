#include "../../lib/syscall.h"
#include "../../include/key_codes.h"

extern int printf(const char *, ...);

int main(void) {
  struct fb_info info;
  fb_info(&info);
  printf("fb: %dx%d pitch=%d bpp=%d\n", (int)info.width, (int)info.height,
         (int)info.pitch, (int)info.bpp);

  unsigned int *fb = (unsigned int *)fb_map();
  if (!fb || info.width <= 0 || info.height <= 0) {
    printf("fb: initialization failed or dimensions are invalid\n");
    return 1;
  }

  // animated gradient
  unsigned long start = get_ticks();
  printf("start ticks %d\n", (int)start);
  while (1) {
    // Poll for keyboard input
    int pressed;
    uint16_t key;
    if (kbd_poll(&pressed, &key)) {
      if (pressed && key == KEY_ESC) {
        printf("We've hit ESC\n");
        return 0;
      }
    }

    long t = get_ticks(); // Get current ticks to animate colors
    // Draw the entire framebuffer
    for (unsigned int y = 0; y < info.height; y++) {
      unsigned int *row = (unsigned int *)((char *)fb + y * info.pitch);
      for (unsigned int x = 0; x < info.width; x++) {
        unsigned int r = (x + t) & 0xFF;
        unsigned int g = (y + t) & 0xFF;
        unsigned int b = (x ^ y) & 0xFF;
        row[x] = (r << 16) | (g << 8) | b; // Color packing
      }
    }
    // Optionally delay or yield to allow for other processes
  }

  printf("gfx done\n");
  return 0;
}
