/* VirtualBox VMMDev display hints + VMware SVGA II scanout support. */
#ifndef VBOX_VIDEO_H
#define VBOX_VIDEO_H

#include <utilities/types.h>

struct vbox_video_mode {
  u64 physical;
  u32 width;
  u32 height;
  u32 pitch;
  u32 bpp;
  u32 red_mask;
  u32 green_mask;
  u32 blue_mask;
};

/* Start the VirtualBox VMMDev/VMSVGA pair at the firmware-selected size. */
int vbox_video_init(u32 width, u32 height);

/* Return the current VMSVGA framebuffer layout. */
int vbox_video_get_mode(struct vbox_video_mode *mode);

/* Poll the newest host size hint and program it. 1=changed, 0=none, -1=error. */
int vbox_video_poll_resize(struct vbox_video_mode *mode);

/* Publish a dirty framebuffer rectangle through the SVGA command FIFO. */
int vbox_video_flush_rect(u32 x, u32 y, u32 width, u32 height);

#endif
