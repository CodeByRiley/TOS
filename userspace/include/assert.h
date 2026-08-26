/* userspace/include/assert.h , assert() stub.
 *
 * No abort path is wired up yet, so assert() is currently a no-op. The
 * macro still consumes its argument so unused-variable warnings stay
 * suppressed in callers that gate logic on assert(...).
 */
#ifndef ASSERT_H
#define ASSERT_H
#define assert(x) ((void)0)
#endif
