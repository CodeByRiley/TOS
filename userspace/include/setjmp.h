/* userspace/include/setjmp.h , non-functional setjmp/longjmp surface.
 *
 * The buffer is sized generously (16 longs) so future real impls fit.
 * Until then, lib/setjmp_stub.c returns 0 from setjmp() and spins in
 * longjmp(). Callers should treat longjmp as terminal.
 */
#ifndef SETJMP_H
#define SETJMP_H

typedef long jmp_buf[16];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
