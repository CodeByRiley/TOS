/* userspace/bin/gfx/gfx.c , fullscreen framebuffer demo.
 *
 * Maps the kernel framebuffer with fb_map() and paints an animated
 * gradient until the user presses ESC. Used to sanity-check the FB
 * syscalls (fb_info, fb_map, fb_damage) and kbd_poll. No WM involvement.
 *
 * fb_map does NOT hand back the scanout. Under virtio-gpu it returns the
 * RAM backbuffer, and the kernel flush thread only transfers rectangles
 * that were marked with fb_damage. Painting without marking damage draws
 * into memory nobody ever pushes to the host , the program runs, responds
 * to keys, exits cleanly, and is invisible the whole time.
 */
#include <lib/syscall.h>
#include <include/key_codes.h>

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

  /* Animation loop: time-varying r/g/b derived from xy + tick counter. */
  unsigned long start = get_ticks();
  printf("start ticks %d\n", (int)start);
  while (1) {
    int pressed;
    uint16_t key;
    if (kbd_poll(&pressed, &key)) {
      if (pressed && key == KEY_ESC) {
        printf("We've hit ESC\n");
        return 0;
      }
    }

    long t = get_ticks();
    for (unsigned int y = 0; y < info.height; y++) {
      unsigned int *row = (unsigned int *)((char *)fb + y * info.pitch);
      for (unsigned int x = 0; x < info.width; x++) {
        unsigned int r = (x + t) & 0xFF;
        unsigned int g = (y + t) & 0xFF;
        unsigned int b = (x ^ y) & 0xFF;
        row[x] = (r << 16) | (g << 8) | b;
      }
    }

    /* Hand the frame to the flush thread. Without this the pixels above
     * never leave the backbuffer. */
    fb_damage(0, 0, (uint32_t)info.width, (uint32_t)info.height);

    /* Full-screen repaint every iteration is already more than the flush
     * thread can consume at PIT rate; yielding keeps this from starving
     * the rest of the system between frames. */
    yield();
  }
}
