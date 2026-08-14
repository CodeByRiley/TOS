/* userspace/lib/setjmp_stub.c — non-functional setjmp/longjmp.
 *
 * Apps that link against setjmp.h compile but can't actually unwind:
 * setjmp() returns 0 once and longjmp() spins forever. DOOM's error path
 * uses it; we accept the hang in lieu of a real save/restore. Replace
 * when a real implementation matters.
 */
#include <include/setjmp.h>

/* Always returns 0 on the (single) "initial" call. */
int setjmp(jmp_buf env) { (void)env; return 0; }

/* Never returns — spins. Caller must treat longjmp() as terminal. */
void longjmp(jmp_buf env, int val) { (void)env; (void)val; for(;;); }
