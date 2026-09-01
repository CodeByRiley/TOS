/* kernel/utilities/string.c , kernel mem... / str... primitives.
 *
 * mem* routines use rep movsq/stosq for the bulk path and a byte tail.
 * str* routines are straightforward byte loops. strdup lives here (vs.
 * stdlib.c) so it sits next to the rest of the str* family.
 */
#include <memory/heap.h>
#include <utilities/string.h>

/* Copy n bytes from src to dst (no overlap). 8-byte chunked. */
void *memcpy(void *dst, const void *src, usize n) {
  void *ret = dst;
  usize q = n >> 3;
  usize b = n & 7;
  __asm__ volatile("cld; rep movsq" : "+D"(dst), "+S"(src), "+c"(q)::"memory");
  __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(b)::"memory");
  return ret;
}

/* Fill n bytes of dst with byte c. 8-byte chunked. */
void *memset(void *dst, int c, usize n) {
  void *ret = dst;
  u64 v = (u8)c;
  v |= v << 8;
  v |= v << 16;
  v |= v << 32;
  usize q = n >> 3;
  usize b = n & 7;
  __asm__ volatile("cld; rep stosq" : "+D"(dst), "+c"(q) : "a"(v) : "memory");
  __asm__ volatile("rep stosb" : "+D"(dst), "+c"(b) : "a"(v) : "memory");
  return ret;
}

/* Overlap-safe memcpy. Forward when dst < src, backward otherwise. */
void *memmove(void *dst, const void *src, usize n) {
  u8 *d = (u8 *)dst;
  const u8 *s = (const u8 *)src;
  if (d == s || n == 0)
    return dst;
  if (d < s)
    return memcpy(dst, src, n);
  /* Backward byte copy. Rare path (console scroll); speed not critical. */
  d += n;
  s += n;
  while (n--)
    *--d = *--s;
  return dst;
}

/* Byte-wise compare. Returns *a-*b at first mismatch. */
int memcmp(const void *a, const void *b, usize n) {
  const u8 *x = (const u8 *)a;
  const u8 *y = (const u8 *)b;
  while (n--) {
    if (*x != *y)
      return *x - *y;
    x++;
    y++;
  }
  return 0;
}

usize strlen(const char *s) {
  usize n = 0;
  while (s[n])
    n++;
  return n;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

/* strncpy(3): pads dst with NULs up to n. Doesn't guarantee NUL term. */
char *strncpy(char *dst, const char *src, usize n) {
  usize i = 0;
  for (; i < n && src[i]; i++)
    dst[i] = src[i];
  for (; i < n; i++)
    dst[i] = 0;
  return dst;
}

char *strcat(char *dst, const char *src) {
  char *d = dst + strlen(dst);
  while (*src)
    *d++ = *src++;
  *d = 0;
  return dst;
}

char *strncat(char *dst, const char *src, usize n) {
  char *d = dst + strlen(dst);
  while (n-- && *src)
    *d++ = *src++;
  *d = 0;
  return dst;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, usize n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  return n == 0 ? 0 : (u8)*a - (u8)*b;
}

static char to_lower_byte(char c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b) {
  while (*a && *b) {
    char ca = to_lower_byte(*a), cb = to_lower_byte(*b);
    if (ca != cb)
      return ca - cb;
    a++;
    b++;
  }
  return (u8)*a - (u8)*b;
}

int strncasecmp(const char *a, const char *b, usize n) {
  while (n && *a && *b) {
    char ca = to_lower_byte(*a), cb = to_lower_byte(*b);
    if (ca != cb)
      return ca - cb;
    a++;
    b++;
    n--;
  }
  return n == 0 ? 0 : (u8)*a - (u8)*b;
}

/* strchr(3). Returns pointer to terminator if c==0, NULL if not found. */
char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  return c == 0 ? (char *)s : 0;
}

/* strlwr(3). Converts string to lowercase in-place. */
char *strlwr(const char *s) {
  usize len = strlen(s);
  char *result = (char *)kmalloc(len + 1);

  if (!result)
    return 0;

  for (usize i = 0; i < len; i++) {
    result[i] = to_lower_byte(s[i]);
  }

  result[len] = '\0';
  return result;
}

/* strrchr(3). Last occurrence of c in s. */
char *strrchr(const char *s, int c) {
  const char *last = 0;
  while (*s) {
    if (*s == (char)c)
      last = s;
    s++;
  }
  if (c == 0)
    return (char *)s;
  return (char *)last;
}

/* strstr(3) , naive O(n*m) substring search. */
char *strstr(const char *h, const char *n) {
  if (!*n)
    return (char *)h;
  for (; *h; h++) {
    const char *a = h;
    const char *b = n;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (!*b)
      return (char *)h;
  }
  return 0;
}

/* strdup(3). Allocates with kmalloc; caller frees with kfree. */
char *strdup(const char *s) {
  usize len = strlen(s) + 1;
  char *p = (char *)kmalloc(len);
  if (!p)
    return 0;
  memcpy(p, s, len);
  return p;
}

/* strerror(3) stub , no errno strings in the kernel. */
char *strerror(int e) {
  (void)e;
  return "error";
}
