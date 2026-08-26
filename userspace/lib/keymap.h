/* userspace/lib/keymap.h , scancode-to-ASCII translation.
 *
 * Used by apps that consume raw key events from msg_get() and need a
 * printable character (e.g., the shell). The kernel reports Linux KEY_*
 * codes; this helper folds shift state into US-QWERTY ASCII.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

/* Translate a Linux KEY_* scancode to ASCII. Returns 0 for non-printable
 * or unmapped keys. `shift` is non-zero when either shift modifier is held. */
char keymap_to_ascii(uint16_t key, int shift);

#endif
