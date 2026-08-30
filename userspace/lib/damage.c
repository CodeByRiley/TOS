#include "damage.h"

void gfx_damage_clear(struct gfx_damage *damage) {
    if (damage)
        damage->rect = gfx_rect_make(0, 0, 0, 0);
}

int gfx_damage_pending(const struct gfx_damage *damage) {
    return damage && !gfx_rect_empty(damage->rect);
}

int gfx_damage_add(struct gfx_damage *damage, struct gfx_rect changed,
                   struct gfx_rect bounds) {
    if (!damage)
        return 0;

    changed = gfx_rect_intersect(changed, bounds);
    if (gfx_rect_empty(changed))
        return 0;

    damage->rect = gfx_rect_union(damage->rect, changed);
    return 1;
}

struct gfx_rect gfx_damage_take(struct gfx_damage *damage) {
    if (!damage)
        return gfx_rect_make(0, 0, 0, 0);

    struct gfx_rect result = damage->rect;
    gfx_damage_clear(damage);
    return result;
}
