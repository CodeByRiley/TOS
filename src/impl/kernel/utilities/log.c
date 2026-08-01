/* src/impl/kernel/utilities/log.c — kernel logging implementation.
 *
 * Each log_write_* wrapper builds a struct log_entry and routes it to
 * both COM1 (serial_*) and the VGA text-mode print writers — that way
 * boot logs are visible to QEMU's serial monitor AND to anyone looking
 * at the display, even before the framebuffer/TTY stack is up.
 *
 * Severity / type values from callers are sanitised through the
 * log_*_from_u8 helpers so an out-of-range value falls back to a sane
 * default rather than indexing past the enum tables.
 */
#include "utilities/log.h"
#include "display/print.h"

/* Validate raw type byte → enum log_type. Returns 1 on hit. */
static int log_type_from_u8(uint8_t value, enum log_type *out) {
    switch (value) {
        case KERNEL:
        case SYSTEM:
        case FILESYS:
        case USER:
            *out = (enum log_type)value;
            return 1;

        default:
            return 0;
    }
}

/* Validate raw level byte → enum log_level. Returns 1 on hit. */
static int log_level_from_u8(uint8_t value, enum log_level *out) {
    switch (value) {
        case LOG_DEBUG:
        case LOG_INFO:
        case LOG_WARN:
        case LOG_ERROR:
        case LOG_FATAL:
            *out = (enum log_level)value;
            return 1;

        default:
            return 0;
    }
}

/* Display label for a subsystem tag. */
static const char *log_type_name(enum log_type type) {
    switch (type) {
        case KERNEL:  return "KERNEL";
        case SYSTEM:  return "SYSTEM";
        case FILESYS: return "FILESYS";
        case USER:    return "USER";
        default:      return "UNKNOWN";
    }
}

/* Bounded string copy. NULs-terminates even when truncating. */
static void log_copy_message(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t i = 0;

    if (dst_cap == 0) return;

    while (src && src[i] != '\0' && i + 1 < dst_cap) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

/* Initialise an entry with safe defaults and copy `message` into it.
 * Bad raw_type/raw_level fall back to SYSTEM / LOG_INFO. */
static void log_init_entry(struct log_entry *entry, const char *message,
                           uint8_t raw_type, uint8_t raw_level) {
    entry->message[0] = '\0';
    entry->timestamp = 0;
    entry->level = LOG_INFO;
    entry->type = SYSTEM;
    entry->has_hex = 0;
    entry->has_int = 0;
    entry->int_value = 0;
    entry->hex_value = 0;
    entry->has_string = 0;
    entry->string_value = 0;

    if (!log_type_from_u8(raw_type, &entry->type)) {
        entry->type = SYSTEM;
    }

    if (!log_level_from_u8(raw_level, &entry->level)) {
        entry->level = LOG_INFO;
    }

    log_copy_message(entry->message, sizeof(entry->message), message);
}

/* Render a populated entry to both serial + VGA text-mode print. */
void log_write_entry(struct log_entry *entry) {
    serial_write_str("[");
    serial_write_str(log_type_name(entry->type));
    serial_write_str("]: ");
    serial_write_str(entry->message);
    if (entry->has_hex) {
        serial_write_str(" ");
        serial_write_hex(entry->hex_value);
    }
    if (entry->has_string) {
        serial_write_str(" ");
        serial_write_str(entry->string_value);
    }
    if (entry->has_int) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)entry->int_value);
        serial_write_str(" ");
        serial_write_str(buf);
    }
    serial_write_str("\n");

    print_write_str("[");
    print_write_str(log_type_name(entry->type));
    print_write_str("]: ");
    print_write_str(entry->message);
    if (entry->has_hex) {
        print_write_str(" ");
        print_write_hex(entry->hex_value);
    }
    if (entry->has_string) {
        print_write_str(" ");
        print_write_str(entry->string_value);
    }
    if (entry->has_int) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)entry->int_value);
        print_write_str(" ");
        print_write_str(buf);
    }
    print_write_str("\n");
}

/* Plain message — no payload. */
void log_write(const char *message, uint8_t raw_type, uint8_t raw_level) {
    struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
    log_write_entry(&entry);
}

/* Message + hex payload. */
void log_write_hex(const char *message, uint64_t value,
                   uint8_t raw_type, uint8_t raw_level) {
    struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
    entry.has_hex = 1;
    entry.hex_value = value;
    log_write_entry(&entry);
}

/* Message + signed-int payload. */
void log_write_int(const char *message, int64_t value,
                   uint8_t raw_type, uint8_t raw_level) {
    struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
    entry.has_int = 1;
    entry.int_value = value;
    log_write_entry(&entry);
}

/* Message + string payload. */
void log_write_string(const char *message, const char *val,
                      uint8_t raw_type, uint8_t raw_level) {
    struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
    entry.has_string = 1;
    entry.string_value = val;
    log_write_entry(&entry);
}

/* Pretty-print a CPU exception. Routed to both serial + VGA so the cause
 * is captured even if the framebuffer pipeline is the thing that broke. */
void log_write_exception(uint64_t int_num, const char *name,
                         uint64_t err_code, uint64_t rip) {
    serial_write_str("[KERNEL]: !! exception ");
    serial_write_hex(int_num);
    serial_write_str(" (");
    serial_write_str(name ? name : "unknown");
    serial_write_str(")\n[KERNEL]:   err=");
    serial_write_hex(err_code);
    serial_write_str("\n[KERNEL]:   rip=");
    serial_write_hex(rip);
    serial_write_str("\n");

    /* A page fault may have been caused by the framebuffer mapping itself.
     * Keep that report on the polled serial path so diagnostics cannot recurse
     * into a second page fault while trying to draw the first one. */
    if (int_num == 14)
        return;

    print_write_str("[KERNEL]: !! exception ");
    print_write_hex(int_num);
    print_write_str(" (");
    print_write_str(name ? name : "unknown");
    print_write_str(")\n[KERNEL]:   err=");
    print_write_hex(err_code);
    print_write_str("\n[KERNEL]:   rip=");
    print_write_hex(rip);
    print_write_str("\n");
}
