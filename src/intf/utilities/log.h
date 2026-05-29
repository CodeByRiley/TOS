#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include "devices/serial.h"
#include "display/print.h"

enum log_level {
	LOG_DEBUG = 0,
	LOG_INFO = 1,
	LOG_WARN = 2,
	LOG_ERROR = 3,
	LOG_FATAL = 4,
};

enum log_type {
	KERNEL = 0,
	SYSTEM = 1,
	USER = 2,
	FILESYS = 3,
};

struct log_entry {
    char message[256];
    uint64_t timestamp;
    enum log_level level;
    enum log_type type;
    uint8_t has_hex;
    uint64_t hex_value;
    uint8_t has_string;
    char* string_value;
};


void log_write(const char* message, uint8_t raw_type, uint8_t raw_level);
void log_write_entry(struct log_entry* entry);
void log_write_exception(uint64_t int_num, const char *name, uint64_t err_code, uint64_t rip);
void log_write_hex(const char* message, uint64_t value, uint8_t raw_type, uint8_t raw_level);
void log_write_string(const char* message, const char* val, uint8_t raw_type, uint8_t raw_level);
#endif
