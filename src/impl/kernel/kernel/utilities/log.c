#include "utilities/log.h"

static int log_type_from_u8(uint8_t value, enum log_type *out) {
    switch (value) {
        case KERNEL:
        case SYSTEM:
        case FILE:
        case USER:
            *out = (enum log_type)value;
            return 1;

        default:
            return 0;
    }
}

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

static const char *log_type_name(enum log_type type) {
    switch (type) {
        case KERNEL: return "KERNEL";
        case SYSTEM: return "SYSTEM";
        case FILE:   return "FILE-SYSTEM";
        case USER:   return "USER";
        default:     return "UNKNOWN";
    }
}

static void log_copy_message(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t i = 0;

    if (dst_cap == 0) return;

    while (src && src[i] != '\0' && i + 1 < dst_cap) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static void log_init_entry(struct log_entry *entry, const char *message,
                           uint8_t raw_type, uint8_t raw_level) {
    entry->message[0] = '\0';
    entry->timestamp = 0;
    entry->level = LOG_INFO;
    entry->type = SYSTEM;
    entry->has_hex = 0;
    entry->hex_value = 0;

    if (!log_type_from_u8(raw_type, &entry->type)) {
        entry->type = SYSTEM;
    }

    if (!log_level_from_u8(raw_level, &entry->level)) {
        entry->level = LOG_INFO;
    }

    log_copy_message(entry->message, sizeof(entry->message), message);
}

void log_write_entry(struct log_entry *entry) {
    serial_write_str("[");
    serial_write_str(log_type_name(entry->type));
    serial_write_str("]: ");
    serial_write_str(entry->message);
    if (entry->has_hex) {
        serial_write_str(" ");
        serial_write_hex(entry->hex_value);
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
    print_write_str("\n");
}

void log_write(const char* message, uint8_t raw_type, uint8_t raw_level) {
	struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
	log_write_entry(&entry);
}

void log_write_hex(const char* message, uint64_t value, uint8_t raw_type, uint8_t raw_level) {
	struct log_entry entry;
    log_init_entry(&entry, message, raw_type, raw_level);
    entry.has_hex = 1;
    entry.hex_value = value;
	log_write_entry(&entry);
}

void log_write_exception(uint64_t int_num, const char *name, uint64_t err_code, uint64_t rip) {
    serial_write_str("[KERNEL]: !! exception ");
    serial_write_hex(int_num);
    serial_write_str(" (");
    serial_write_str(name ? name : "unknown");
    serial_write_str(")\n[KERNEL]:   err=");
    serial_write_hex(err_code);
    serial_write_str("\n[KERNEL]:   rip=");
    serial_write_hex(rip);
    serial_write_str("\n");

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
