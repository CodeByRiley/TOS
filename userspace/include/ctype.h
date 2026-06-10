/* userspace/include/ctype.h — ASCII-only character classifiers.
 *
 * All routines are static inlines; no .c file needed. Locale is ignored —
 * everything treats input as 7-bit ASCII.
 */
#ifndef CTYPE_H
#define CTYPE_H

/* '0'..'9'. */
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
/* Standard C whitespace set. */
static inline int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
/* ASCII letter, either case. */
static inline int isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
/* Letter or digit. */
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
/* Upper-case letter. */
static inline int isupper(int c) { return c>='A'&&c<='Z'; }
/* Lower-case letter. */
static inline int islower(int c) { return c>='a'&&c<='z'; }
/* Printable ASCII (space..~). */
static inline int isprint(int c) { return c>=' '&&c<=126; }
/* Hex digit. */
static inline int isxdigit(int c){ return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
/* Control character (NUL..US, DEL). */
static inline int iscntrl(int c) { return c<32 || c==127; }
/* Printable but not alphanumeric or whitespace. */
static inline int ispunct(int c) { return isprint(c) && !isalnum(c) && !isspace(c); }
/* Fold to lower-case if upper, otherwise pass through. */
static inline int tolower(int c) { return (c>='A'&&c<='Z')?c+32:c; }
/* Fold to upper-case if lower, otherwise pass through. */
static inline int toupper(int c) { return (c>='a'&&c<='z')?c-32:c; }

#endif
