/* userspace/lib/syscall.h , userspace syscall interface.
 *
 * The shared syscall registry and payload structs, plus the C wrappers
 * that userspace apps call instead of writing `syscall0..6` directly.
 *
 * Sections:
 *   - SYS_* numbers           , selected from kernel/arch/syscalls.def.
 *   - syscall ABI payloads    , imported from kernel/arch/syscall_abi.h.
 *   - syscallN()              , raw register-passing trampolines.
 *   - libc-style wrappers     , typed helpers around the syscalls above.
 *
 * Higher-level winman client API lives in lib/wm.h, not here.
 */
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <arch/syscall_abi.h>
#include <stddef.h>
#include <stdint.h>

#ifdef TOS_USE_MUSL
/* Built against musl: the POSIX surface comes from the real headers, and
 * the TOS declarations of those same functions are compiled out below.
 * Pulling them in here means every lib/ source keeps working unchanged ,
 * bmp.c calling open()/read() gets musl's, which issue the same syscalls. */
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---------------- System-call numbers -----------------------------------
 *
 * The kernel owns the registry. BOTH entries become this enum; Linux-only
 * compatibility calls are intentionally absent because musl supplies those
 * numbers itself. */
enum syscall_number {
#define SYSCALL_BOTH(name, number) SYS_##name = number,
#define SYSCALL_KERNEL(name, number)
#define SYSCALL_ALIAS_BOTH(name, target) SYS_##name = SYS_##target,
#include <arch/syscalls.def>
#undef SYSCALL_ALIAS_BOTH
#undef SYSCALL_KERNEL
#undef SYSCALL_BOTH
};

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define AUDIO_CHANNELS_STEREO 2

#define AUDIO_ERR_NO_DEVICE (-1)
#define AUDIO_ERR_BUSY (-2)
#define AUDIO_ERR_INVALID (-3)
#define AUDIO_ERR_NOT_OWNER (-4)

/* ---------------- mmap / mprotect --------------------------------------
 *
 * Linux PROT_ and MAP_ values. Two deliberate deviations from Linux:
 *
 *   - MAP_FIXED FAILS if any page in the range is already mapped, rather
 *     than replacing it. A loader probing its preferred image base gets
 *     "taken, go relocate" instead of a silently clobbered mapping.
 *   - PROT_NONE is rejected. Mappings reserve virtual space but are not
 *     backed until accessed, but there is no use-case for mapping a region
 *     as completely inaccessible, so PROT_NONE is still an error.
 *
 * Mappings are always anonymous, private, and zero-filled. There is no
 * file-backed mapping: map the range, then read() into it.
 *
 * musl's <sys/mman.h> defines the same names with the same values, so a
 * musl-linked translation unit takes them from there and redefining them
 * here is pure -Wmacro-redefined noise. */
#ifndef TOS_USE_MUSL
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)
#endif

/* Mouse-button bits carried in struct msg.param. */
#define MOUSE_BTN_LEFT 0x01
#define MOUSE_BTN_RIGHT 0x02
#define MOUSE_BTN_MIDDLE 0x04
#define MOUSE_BTN_FORWARD 0x08
#define MOUSE_BTN_BACK 0x10

/* ---------------- Raw syscall trampolines -------------------------------
 *
 * Everything crossing the syscall boundary is sysarg_t, never `long`.
 * The ELF toolchain is LP64 so the two are the same width there, but the
 * PE variants are built by mingw, which is LLP64: `long` is four bytes.
 * A pointer passed as long would reach the kernel truncated to its low 32
 * bits , for a PE image based at 0x140000000, an address 4 GiB from the
 * one intended. long long is 64-bit under both models.
 *
 * ELF builds get these from lib/syscall.s; PE builds from lib/syscall_pe.c.
 * Userspace code normally calls the typed wrappers below instead. */
typedef long long sysarg_t;

sysarg_t syscall0(sysarg_t n);
sysarg_t syscall1(sysarg_t n, sysarg_t a);
sysarg_t syscall2(sysarg_t n, sysarg_t a, sysarg_t b);
sysarg_t syscall3(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c);
sysarg_t syscall4(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c, sysarg_t d);
sysarg_t syscall5(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c, sysarg_t d,
                  sysarg_t e);
sysarg_t syscall6(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c, sysarg_t d,
                  sysarg_t e, sysarg_t f);

/* Pointer -> syscall argument, at full width under both data models. */
#define SYSPTR(p) ((sysarg_t)(uintptr_t)(p))

/* ---------------- Typed libc-style wrappers ----------------------------- */
/* Standard POSIX-ish file / process surface. */
/* These fourteen are also defined by musl, with different prototypes
 * (musl's open() is variadic, its exit() is _Noreturn, and so on). A
 * translation unit built against musl takes musl's declarations from the
 * real headers; declaring TOS's here as well is a hard conflict, so they
 * are compiled out whenever TOS_USE_MUSL is set. Implementations live in
 * syscall_posix.c, which musl-linked binaries do not link. */
#ifndef TOS_USE_MUSL
long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
long open(const char *path, int flags);
long close(int fd);
long lseek(int fd, long off, int whence);
long readdir(unsigned *index, char *buf, size_t n);
long unlink(const char *path);
void exit(int code);
long chdir(const char *path);
char *getcwd(char *buf, size_t size);
#endif

long readdir_path(const char *path, unsigned *index, char *buf, size_t n);
long mkdir_path(const char *path);
long rmdir_path(const char *path);
long yield(void);

/* ---------------- Mounting ---------------------------------------------
 *
 * blockdev_list fills `out` with up to `max` volumes the storage drivers
 * published and returns how many it wrote, or -1. A full buffer may mean
 * there are more, so retry with a bigger one. The .name is what mount()
 * wants as its source.
 *
 * mount() and umount() are musl's prototypes at Linux's syscall numbers, so
 * a musl-linked binary calls musl's wrappers and lands in the same place.
 * Two TOS-specific rules apply either way:
 *
 *   - `source` is a published volume name ("ahci0"), not a device node. A
 *     "/dev/" prefix is accepted and ignored. There is no loop device, so a
 *     file cannot be a source.
 *   - `filesystemtype` may be NULL or "" to probe for the format, while
 *     `mountflags` must be 0 and `data` NULL: no flag or mount option is
 *     honoured yet, and accepting one silently would be a lie.
 *
 * Failures are reported the Linux way, as a negated errno: ENOENT for a
 * volume name nothing answers to, ENODEV for a filesystem type we do not
 * have, EBUSY for a mountpoint already in use (and for unmounting "/", or a
 * volume with a file still open), EINVAL for a source no backend recognises,
 * EIO when the final sync of an unmount failed and the volume therefore
 * stays mounted. A musl-linked caller sees the usual errno and -1, because
 * musl's __syscall_ret does that translation; the hand-rolled wrappers in
 * syscall_posix.c hand the negative value back unchanged, as everything else
 * in that file does.
 *
 * The one worth recognising is EINVAL on a whole disk: every backend read it
 * and refused, which usually means it holds a partition table rather than a
 * filesystem. Mount one of its partitions instead.
 *
 * Neither call needs a privilege the caller might lack, because there is no
 * privilege model to check yet. */
long blockdev_list(struct blockdev_info *out, long max);
/* Raw I/O is for maintenance tools. A request holds at most 64 512-byte
 * sectors, and writes to mounted volumes are rejected by the kernel. */
long blockdev_read(const char *source, uint64_t lba, uint32_t sectors,
                   void *out);
long blockdev_write(const char *source, uint64_t lba, uint32_t sectors,
                    const void *in);
long blockdev_flush(const char *source);
long fs_sync(void);
#ifndef TOS_USE_MUSL
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);
#endif

/* Raw metadata straight from the kernel. POSIX stat()/fstat() in
 * <sys/stat.h> wrap these. */
long stat_raw(const char *path, struct stat_user *out);
long fstat_raw(int fd, struct stat_user *out);

/* Anonymous, private, zero-filled, demand-paged.
 *
 * addr is a request, honoured only with MAP_FIXED , and MAP_FIXED fails
 * rather than replacing an existing mapping. Returns MAP_FAILED on error.
 *
 * Physical memory is NOT allocated immediately. The kernel only reserves
 * the virtual address space. Physical RAM is allocated one page at a time
 * by the page fault handler when the program actually reads or writes to it.
 */
#ifndef TOS_USE_MUSL
void *mmap(void *addr, size_t len, int prot, int flags);
int mprotect(void *addr, size_t len, int prot);
int munmap(void *addr, size_t len);
#endif

/* Input + windowing. */
long msg_get(struct msg *out);
long msg_peek(struct msg *out);
long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons);
long fb_info(struct fb_info *out);
void *fb_map(void);
long fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
long fb_present(const void *pixels, uint32_t pitch, const struct fb_rect *rects,
                uint32_t rect_count);
long fb_register(const void *pixels, uint32_t pitch);
long fb_unregister(void);
long kbd_poll(int *pressed, uint16_t *key);
long get_ticks(void);

/* Process control. exec() blocks until child exits; spawn() returns the
 * child's pid immediately so the caller (e.g., the shell) stays free. */
long exec(const char *path, char *const argv[]);
long spawn(const char *path, char *const argv[]);
#ifndef TOS_USE_MUSL
long kill(long pid, int signal);
#endif
long sys_shutdown(int time, const char *reason);
long sys_reboot(int time);

/* Console (TTY) surface. push/pop give fullscreen apps an alt-screen. */
long con_write(const char *buf, size_t n);
long con_clear(void);
long con_push(void);
long con_pop(void);
long con_zoom(long delta);
long sleep_ticks(unsigned long n);
long get_pid(void);

/* Raw clock read. clock_id is CLOCK_REALTIME (0) or CLOCK_MONOTONIC (1);
 * `ts` points at a struct timespec. Prefer clock_gettime() from
 * <include/time.h>, which types the argument properly. */
long sys_clock_gettime(int clock_id, void *ts);
/* Push one ASCII character into TTY channel `tty`'s input ring. Winman
 * calls this for the console window that has focus; the channel is named
 * explicitly because winman itself is on a different one. */
void tty_inject(int tty, char c);

/* Drain characters winman injected for this process's console. Non-blocking;
 * returns the count read, 0 when nothing is queued. The channel is implicit:
 * it is always the caller's own (task->tty, inherited at spawn).
 *
 * Console applications must read here rather than poll kbd_poll: the raw
 * keyboard ring is filled for every keystroke regardless of window focus,
 * so polling it collects whatever the user types into other windows too. */
long tty_read_input(char *buf, unsigned long max);

/* Audio output. audio_write is non-blocking and can return a short count or
 * zero when the ring is full. Input must be frame-aligned S16-LE stereo PCM.
 * audio_drain blocks until queued samples finish; audio_close discards them. */
long audio_open(uint32_t sample_rate, uint32_t channels, uint32_t format);
long audio_write(const void *pcm, size_t bytes);
long audio_status(struct audio_status *out);
long audio_drain(void);
long audio_close(void);
long audio_set_volume(int percent);
long audio_pause(void);
long audio_resume(void);

/* IPC and shared memory primitives. The kernel fills in from_pid on the
 * receiver side; senders may leave it zero. */
long ipc_send(int target_pid, const struct ipc_msg *m);
long ipc_recv(struct ipc_msg *out);
long shmem_share(int target_pid, uint64_t in_va, long npages,
                 uint64_t *out_target_va);
long shmem_unshare(int target_pid, uint64_t in_va, long npages);

/* Winman registration / discovery. The pid of the WM is not cached because
 * winman is allowed to crash and respawn. */
long wm_register(void);
long wm_pid(void);
/* Read TTY channel `tty`'s pending output. Winman calls this once per live
 * console window per frame and renders what comes back. */
long tty_drain(int tty, char *buf, long max);

/* Console multiplexing, restricted to the process holding the WM role.
 *
 * tty_alloc claims a spare channel (1..TTY_MAX-1) and returns its index, or
 * -1 when they are all taken. tty_free gives one back and discards whatever
 * was still buffered on it. tty_spawn is spawn() with the child pinned to a
 * channel rather than inheriting the caller's , without it a shell launched
 * by winman would land on winman's own console. */
long tty_alloc(void);
long tty_free(int tty);
long tty_spawn(const char *path, char *const argv[], int tty);

/* Diagnostics. proc_list returns the number of rows filled; mem_stats
 * returns 0 on success. */
long proc_list(struct proc_info *out, long max);
long mem_stats(struct mem_stats *out);

/* NIC counters and captured frames. net_stats returns 0; net_capture
 * returns how many frames it wrote and advances *cursor past them, so a
 * caller polls with the same cursor forever. Start it at 0 to replay
 * whatever the ring still holds, or at stats.seq_next to begin live.
 *
 * A cursor that falls behind the ring is moved forward to the oldest
 * frame still held rather than failing; compare out[0].seq with the input
 * cursor to count missed frames. */
long net_stats(struct net_stats *out);
long net_capture(uint64_t *cursor, struct net_frame *out, long max);
/* One echo request, one reply. 0 on success with req->rtt_ms filled in,
 * -1 on timeout or a rejected argument, -2 when there is no route or too
 * many pings are already in flight. Blocks for up to req->timeout_ms. */
long net_ping(struct net_ping *req);

/* Threading */
long thread_create(void *(*entry)(void *), void *stack, void *arg);
long thread_exit(void);
long thread_join(long tid);
long futex_wait(uint32_t *addr, uint32_t expected);
long futex_wake(uint32_t *addr);

#endif
