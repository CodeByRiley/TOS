/* kernel/utilities/log.h — kernel logging surface.
 *
 * Tagged log records are timestamped (PIT ticks) and routed to serial +
 * optionally an in-memory ring. Severity is `enum log_level`; subsystem
 * is `enum log_type`. The richer log_write_entry() lets callers attach a
 * single hex / int / string payload to the message.
 *
 * Implementation: kernel/utilities/log.c.
 */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stdarg.h>
#include <devices/serial.h>
#include <display/print.h>
#include <utilities/types.h>
#include <utilities/printf.h>

/* Severity. Filtering uses >= comparison: callers can set a floor and
 * lower-severity events get dropped. */
enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4,
};

/* Subsystem tag, used as a per-source prefix on serial output. */
enum log_type {
    KERNEL  = 0,
    SYSTEM  = 1,
    USER    = 2,
    FILESYS = 3,
};

/* Generic log record. Only one of the optional payload fields is meant
 * to be populated at a time; the `has_*` flags pick which one renders. */
struct log_entry {
    char           message[256];
    uint64_t       timestamp;
    enum log_level level;
    enum log_type  type;
    uint8_t        has_string;
    uint8_t        has_hex;
    uint8_t        has_int;
    uint64_t       hex_value;
    const char    *string_value;
    int64          int_value;
};

/* Plain message — no payload. */
void log_write(const char *message, uint8_t raw_type, uint8_t raw_level);

/* Rich record. Caller fills in `entry` (including has_* flags). */
void log_write_entry(struct log_entry *entry);

/* CPU exception logger — pretty-prints the IDT vector, name, error code,
 * and faulting RIP. Page faults are serial-only because framebuffer output
 * may itself be the failing mapping. */
void log_write_exception(uint64_t int_num, const char *name,
                         uint64_t err_code, uint64_t rip);

/* Convenience wrappers that produce a single-payload entry. */
void log_write_hex(const char *message, uint64_t value,
                   uint8_t raw_type, uint8_t raw_level);
void log_write_int(const char *message, int64_t value,
                   uint8_t raw_type, uint8_t raw_level);
void log_write_string(const char *message, const char *val,
                      uint8_t raw_type, uint8_t raw_level);

/* printf-style logging */
void log_write_fmt(uint8_t raw_type, uint8_t raw_level, const char *fmt, ...);
void log_write_vfmt(uint8_t raw_type, uint8_t raw_level, const char *fmt, va_list args);

#endif
