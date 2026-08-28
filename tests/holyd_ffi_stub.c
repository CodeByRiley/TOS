/* tests/holyd_ffi_stub.c - host stand-in for userspace/bin/holyd/ffi.c.
 *
 * compiler.c calls ffi_lookup_native to resolve names like UdpSocket to a
 * native function. The real implementation lives in ffi.c, which includes
 * <sys/socket.h> and <netinet/in.h> -- headers MinGW does not have, so it
 * cannot be part of a host build at all.
 *
 * The host test exercises the compiler, not the network FFI, so resolving
 * every name to "not a native" is the correct answer here rather than a
 * convenient one: it is exactly what the compiler sees for any identifier
 * that is not a registered native, and that path is the one the test cares
 * about. A test that linked the real FFI would be testing sockets on
 * Windows, which is not a thing this project needs to work.
 *
 * If holyd ever gains natives whose compilation differs from ordinary
 * calls, this stub stops being sufficient and the test needs a real table
 * of host-safe natives instead.
 */
#include "../userspace/bin/holyd/src/ffi.h"

NativeFn ffi_lookup_native(const char *name, int len) {
  (void)name;
  (void)len;
  return 0;
}

/* The real one predefines the windowing constants. Defining nothing is the
 * honest host answer for the same reason: a test program that names EV_QUIT
 * should fail to resolve it, because on the host there is no window manager
 * to have produced it. */
void ffi_define_globals(Environment *env) { (void)env; }
