/* Bounded rectangular damage tracking for retained pixel surfaces. */
#ifndef DAMAGE_H
#define DAMAGE_H

#include "gfx.h"

struct gfx_damage {
    struct gfx_rect rect;
};

void gfx_damage_clear(struct gfx_damage *damage);
int gfx_damage_pending(const struct gfx_damage *damage);

/* Add a changed rectangle, clipped to bounds. Returns non-zero when any
 * pixels survived clipping. Multiple additions are accumulated as one
 * bounding rectangle, which is cheap and is suitable for partial repaint. */
int gfx_damage_add(struct gfx_damage *damage, struct gfx_rect changed,
                   struct gfx_rect bounds);

/* Return the accumulated rectangle and reset the tracker. */
struct gfx_rect gfx_damage_take(struct gfx_damage *damage);

#endif
