#include "process.h"

#include <string.h>

static int has_separator(const char *text) {
    for (; text && *text; text++)
        if (*text == '/' || *text == '\\')
            return 1;
    return 0;
}

static int final_component_has_dot(const char *text) {
    const char *component = text;
    for (const char *p = text; p && *p; p++) {
        if (*p == '/' || *p == '\\')
            component = p + 1;
    }
    for (; component && *component; component++)
        if (*component == '.')
            return 1;
    return 0;
}

static int append(char *out, size_t capacity, size_t *length,
                  const char *text, size_t text_length) {
    if (*length + text_length + 1 > capacity)
        return -1;
    memcpy(out + *length, text, text_length);
    *length += text_length;
    out[*length] = 0;
    return 0;
}

static int make_candidate(char *out, size_t capacity,
                          const char *directory, size_t directory_length,
                          const char *program, const char *suffix) {
    size_t length = 0;
    if (directory_length > 0) {
        if (append(out, capacity, &length, directory, directory_length) != 0)
            return -1;
        if (out[length - 1] != '/' &&
            append(out, capacity, &length, "/", 1) != 0)
            return -1;
    }
    if (append(out, capacity, &length, program, strlen(program)) != 0)
        return -1;
    if (suffix && append(out, capacity, &length, suffix, strlen(suffix)) != 0)
        return -1;
    return 0;
}

static int candidate_exists(const char *candidate) {
    struct stat_user metadata;
    return stat_raw(candidate, &metadata) == 0 &&
           metadata.type == STAT_TYPE_FILE;
}

static int probe(char *out, size_t capacity, const char *directory,
                 size_t directory_length, const char *program) {
    static const char *const suffixes[] = {"", ".elf", ".exe"};
    int count = final_component_has_dot(program) ? 1 : 3;
    for (int i = 0; i < count; i++) {
        if (make_candidate(out, capacity, directory, directory_length,
                           program, suffixes[i]) == 0 &&
            candidate_exists(out))
            return 0;
    }
    if (capacity)
        out[0] = 0;
    return -1;
}

int process_resolve(const char *program, const char *search_path,
                    char *out, size_t capacity) {
    if (!program || !program[0] || !out || capacity == 0)
        return -1;
    out[0] = 0;

    if (has_separator(program))
        return probe(out, capacity, 0, 0, program);

    if (!search_path || !search_path[0])
        search_path = PROCESS_DEFAULT_PATH;

    const char *cursor = search_path;
    for (;;) {
        const char *end = cursor;
        while (*end && *end != ':')
            end++;
        if (probe(out, capacity, cursor, (size_t)(end - cursor), program) == 0)
            return 0;
        if (!*end)
            break;
        cursor = end + 1;
    }
    return -1;
}

long process_spawn(const char *program, char *const argv[],
                   const char *search_path) {
    char resolved[PROCESS_PATH_MAX];
    if (process_resolve(program, search_path, resolved, sizeof(resolved)) != 0)
        return -1;
    return spawn(resolved, argv);
}

long process_exec(const char *program, char *const argv[],
                  const char *search_path) {
    char resolved[PROCESS_PATH_MAX];
    if (process_resolve(program, search_path, resolved, sizeof(resolved)) != 0)
        return -1;
    return exec(resolved, argv);
}

long process_snapshot(struct proc_info *out, size_t capacity) {
    if (!out || capacity == 0)
        return -1;
    if (capacity > 0x7fffffffu)
        capacity = 0x7fffffffu;
    return proc_list(out, (long)capacity);
}

int process_get(int pid, struct proc_info *out) {
    if (pid <= 0)
        return -1;
    struct proc_info processes[256];
    long count = process_snapshot(processes,
                                  sizeof(processes) / sizeof(processes[0]));
    if (count < 0)
        return -1;
    for (long i = 0; i < count; i++) {
        if (processes[i].pid != pid)
            continue;
        if (out)
            *out = processes[i];
        return 1;
    }
    return 0;
}

int process_is_alive(int pid) {
    struct proc_info process;
    int found = process_get(pid, &process);
    if (found <= 0)
        return found;
    return process.state != PROC_STATE_ZOMBIE &&
           process.state != PROC_STATE_DEAD;
}

int process_wait(int pid, uint32_t timeout_ticks, struct proc_info *last_seen) {
    if (pid <= 0)
        return PROCESS_ERROR;
    uint32_t started = (uint32_t)get_ticks();
    for (;;) {
        struct proc_info process;
        int found = process_get(pid, &process);
        if (found < 0)
            return PROCESS_ERROR;
        if (found == 0)
            return PROCESS_EXITED;
        if (last_seen)
            *last_seen = process;
        if (process.state == PROC_STATE_ZOMBIE ||
            process.state == PROC_STATE_DEAD)
            return PROCESS_EXITED;
        if (timeout_ticks != PROCESS_WAIT_FOREVER &&
            (uint32_t)((uint32_t)get_ticks() - started) >= timeout_ticks)
            return PROCESS_TIMEOUT;
        sleep_ticks(1);
    }
}
