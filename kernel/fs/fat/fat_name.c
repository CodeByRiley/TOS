/* VFAT long names and DOS 8.3 aliases share one codec for both layouts. */
#include "fat_internal.h"
#include <utilities/string.h>

/* Byte offsets of the 13 characters stored in one long-name slot. */
static const u8 lfn_offsets[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};


/* Long-filename helpers. */

u8 lfn_checksum(const char *name) {
  u8 sum = 0;
  for (int i = 0; i < 11; i++)
    sum = (u8)(((sum & 1) << 7) + (sum >> 1) + (u8)name[i]);
  return sum;
}

void lfn_reset(struct lfn_state *state) {
  state->length = 0;
  state->valid = 0;
  state->expect = 0;
  state->checksum = 0;
  state->start_index = 0;
}

void lfn_feed(struct lfn_state *state, const struct dir_entry *entry,
                     u32 index) {
  const u8 *raw = (const u8 *)entry;
  u8 marker = raw[0];

  if (marker == 0xE5) {
    lfn_reset(state);
    return;
  }

  u8 seq = marker & 0x1F;
  int last = (marker & 0x40) != 0;
  if (seq == 0 || seq > FAT_LFN_MAX_SLOTS) {
    lfn_reset(state);
    return;
  }

  if (last) {
    lfn_reset(state);
    state->valid = 1;
    state->expect = seq;
    state->checksum = raw[13];
    state->length = (u32)seq * 13;
    state->start_index = index;
  } else if (!state->valid || seq != state->expect || raw[13] != state->checksum) {
    lfn_reset(state);
    return;
  }

  u32 base = (u32)(seq - 1) * 13;
  for (int i = 0; i < 13; i++) {
    u16 ch = (u16)(raw[lfn_offsets[i]] | ((u16)raw[lfn_offsets[i] + 1] << 8));
    char out;
    if (ch == 0x0000 || ch == 0xFFFF)
      out = '\0';
    else if (ch < 0x80)
      out = (char)ch;
    else
      out = '_';
    state->name[base + i] = out;
  }

  state->expect = (u8)(seq - 1);
}

const char *lfn_take(struct lfn_state *state,
                            const struct dir_entry *entry) {
  if (!state->valid || state->expect != 0)
    return 0;
  if (lfn_checksum(entry->name) != state->checksum)
    return 0;

  u32 length = 0;
  while (length < state->length && state->name[length] != '\0')
    length++;
  if (length == 0 || length > 255)
    return 0;
  state->name[length] = '\0';
  return state->name;
}

void write_lfn_slot(struct dir_entry *slot, const char *name,
                           u32 length, u8 seq, int last, u8 checksum) {
  u8 *raw = (u8 *)slot;
  memset(raw, 0, sizeof(*slot));
  raw[0] = (u8)(seq | (last ? 0x40 : 0));
  raw[11] = FAT_ATTR_LFN;
  raw[13] = checksum;

  u32 base = (u32)(seq - 1) * 13;
  for (int i = 0; i < 13; i++) {
    u32 idx = base + (u32)i;
    u16 ch;
    if (idx < length)
      ch = (u16)(u8)name[idx];
    else if (idx == length)
      ch = 0x0000;
    else
      ch = 0xFFFF;
    raw[lfn_offsets[i]] = (u8)(ch & 0xFF);
    raw[lfn_offsets[i] + 1] = (u8)(ch >> 8);
  }
}

/* Short-name helpers. */
static int valid_short_char(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9'))
    return 1;
  switch (c) {
  case '$': case '%': case '\'': case '-': case '_':
  case '@': case '~': case '`': case '!': case '(':
  case ')': case '{': case '}': case '^': case '#': case '&':
    return 1;
  default:
    return 0;
  }
}

static char to_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }

static void split_extension(const char *name, u32 length,
                            u32 *base_len, u32 *ext_at, u32 *ext_len) {
  u32 dot = length;
  for (u32 i = length; i > 1; i--) {
    if (name[i - 1] == '.') {
      dot = i - 1;
      break;
    }
  }
  *base_len = dot;
  *ext_at = dot < length ? dot + 1 : length;
  *ext_len = length - *ext_at;
}

int short_name_exact(const char *name, u32 length,
                            char out[11], u8 *nt_case) {
  if (length == 0 || length > 12 || name[length - 1] == '.')
    return -1;

  u32 base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);
  if (base_len == 0 || base_len > 8 || ext_len > 3)
    return -1;

  int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
  for (u32 i = 0; i < base_len; i++) {
    if (!valid_short_char(name[i]))
      return -1;
    if (is_lower(name[i])) base_lower = 1;
    if (is_upper(name[i])) base_upper = 1;
  }
  for (u32 i = 0; i < ext_len; i++) {
    char c = name[ext_at + i];
    if (!valid_short_char(c))
      return -1;
    if (is_lower(c)) ext_lower = 1;
    if (is_upper(c)) ext_upper = 1;
  }
  if ((base_lower && base_upper) || (ext_lower && ext_upper))
    return -1;

  memset(out, ' ', 11);
  for (u32 i = 0; i < base_len; i++)
    out[i] = to_upper(name[i]);
  for (u32 i = 0; i < ext_len; i++)
    out[8 + i] = to_upper(name[ext_at + i]);

  if ((u8)out[0] == 0xE5)
    out[0] = 0x05;

  *nt_case = (u8)((base_lower ? FAT_CASE_BASE_LOWER : 0) |
                       (ext_lower ? FAT_CASE_EXT_LOWER : 0));
  return 0;
}

int short_name_alias(struct fat_dir dir, const char *name,
                            u32 length, char out[11]) {
  u32 base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);

  char base[8];
  u32 base_used = 0;
  for (u32 i = 0; i < base_len && base_used < 8; i++) {
    char c = name[i];
    if (c == ' ' || c == '.')
      continue;
    base[base_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }
  if (base_used == 0)
    base[base_used++] = '_';

  char ext[3];
  u32 ext_used = 0;
  for (u32 i = 0; i < ext_len && ext_used < 3; i++) {
    char c = name[ext_at + i];
    if (c == ' ' || c == '.')
      continue;
    ext[ext_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }

  for (u32 n = 1; n <= 999999; n++) {
    char suffix[7];
    u32 suffix_len = 0;
    for (u32 value = n; value; value /= 10)
      suffix[suffix_len++] = (char)('0' + value % 10);

    u32 stem = 8 - (suffix_len + 1);
    if (stem > base_used)
      stem = base_used;

    memset(out, ' ', 11);
    for (u32 i = 0; i < stem; i++)
      out[i] = base[i];
    out[stem] = '~';
    for (u32 i = 0; i < suffix_len; i++)
      out[stem + 1 + i] = suffix[suffix_len - 1 - i];
    for (u32 i = 0; i < ext_used; i++)
      out[8 + i] = ext[i];

    if (!short_name_taken(dir, out))
      return 0;
  }
  return -1;
}

u32 entry_short_name(const struct dir_entry *entry, char *out) {
  u32 length = 0;
  int base_lower = (entry->nt_case & FAT_CASE_BASE_LOWER) != 0;
  int ext_lower = (entry->nt_case & FAT_CASE_EXT_LOWER) != 0;

  for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
    char c = entry->name[i];
    if (i == 0 && (u8)c == 0x05)
      c = (char)0xE5;
    out[length++] = base_lower && is_upper(c) ? (char)(c + ('a' - 'A')) : c;
  }

  int has_extension = 0;
  for (int i = 0; i < 3; i++) {
    if (entry->ext[i] != ' ') {
      has_extension = 1;
      break;
    }
  }
  if (has_extension) {
    out[length++] = '.';
    for (int i = 0; i < 3 && entry->ext[i] != ' '; i++) {
      char c = entry->ext[i];
      out[length++] = ext_lower && is_upper(c) ? (char)(c + ('a' - 'A')) : c;
    }
  }
  out[length] = '\0';
  return length;
}
