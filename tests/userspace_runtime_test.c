/* Host regressions for libevent, libapp, libprocess, and libwm integration. */
#include "app.h"
#include "event.h"
#include "process.h"

#include <string.h>

extern int printf(const char *format, ...);

static int failed;

static void expect(int condition, const char *message) {
    if (condition)
        return;
    printf("FAIL: %s\n", message);
    failed = 1;
}

/* Fake kernel IPC queue. */
static struct ipc_msg kernel_queue[64];
static int queue_head;
static int queue_tail;
static uint32_t ticks;
static uint32_t last_sent_type;
static char last_launched[PROCESS_PATH_MAX];
static uint32_t fake_surface[80 * 60];

static void queue_message(uint32_t type, int from_pid) {
    struct ipc_msg message;
    memset(&message, 0, sizeof(message));
    message.type = type;
    message.from_pid = (uint32_t)from_pid;
    kernel_queue[queue_tail++] = message;
}

long ipc_recv(struct ipc_msg *out) {
    if (queue_head >= queue_tail)
        return 0;
    *out = kernel_queue[queue_head++];
    return 1;
}

long ipc_send(int target_pid, const struct ipc_msg *message) {
    last_sent_type = message->type;
    if (target_pid == 42 && message->type == IPC_WM_CREATE_REQ) {
        struct ipc_msg response;
        memset(&response, 0, sizeof(response));
        response.type = IPC_WM_CREATE_RESP;
        response.from_pid = 42;
        response.a = 7;
        response.va = (uint64_t)(uintptr_t)fake_surface;
        response.pitch = 80 * 4;
        kernel_queue[queue_tail++] = response;
    }
    return 0;
}

long wm_pid(void) { return 42; }
long get_ticks(void) { return (long)ticks; }
long sleep_ticks(unsigned long count) {
    ticks += (uint32_t)count;
    return 0;
}

void gfx_surface_init(struct gfx_surface *surface, uint32_t *pixels,
                      int width, int height, int stride) {
    surface->px = pixels;
    surface->w = width;
    surface->h = height;
    surface->stride = stride;
    surface->clip = gfx_rect_make(0, 0, width, height);
}

/* Fake filesystem/process table. */
long stat_raw(const char *path, struct stat_user *out) {
    static const char *const files[] = {
        "/usr/bin/hello.elf",
        "/system/bin/tool.exe",
        "relative/app.elf",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (strcmp(path, files[i]) == 0) {
            if (out)
                memset(out, 0, sizeof(*out));
            return 0;
        }
    }
    if (strcmp(path, "/usr/bin/folder") == 0) {
        if (out) {
            memset(out, 0, sizeof(*out));
            out->type = STAT_TYPE_DIR;
        }
        return 0;
    }
    return -1;
}

long spawn(const char *path, char *const argv[]) {
    (void)argv;
    strncpy(last_launched, path, sizeof(last_launched) - 1);
    last_launched[sizeof(last_launched) - 1] = 0;
    return 123;
}

long exec(const char *path, char *const argv[]) {
    (void)argv;
    strncpy(last_launched, path, sizeof(last_launched) - 1);
    last_launched[sizeof(last_launched) - 1] = 0;
    return 17;
}

enum table_mode {
    TABLE_ABSENT,
    TABLE_RUNNING,
    TABLE_ZOMBIE,
};
static enum table_mode process_table_mode;

long proc_list(struct proc_info *out, long maximum) {
    if (!out || maximum <= 0 || process_table_mode == TABLE_ABSENT)
        return process_table_mode == TABLE_ABSENT ? 0 : -1;
    memset(out, 0, sizeof(*out));
    out->pid = 123;
    out->state = process_table_mode == TABLE_ZOMBIE
                     ? PROC_STATE_ZOMBIE
                     : PROC_STATE_RUNNING;
    return 1;
}

static void test_event_filtering(void) {
    const uint32_t custom_type = IPC_USER_FIRST + 9;
    queue_message(custom_type, 77);
    queue_message(IPC_WM_INPUT, 42);
    kernel_queue[queue_tail - 1].a = WM_EV_KEY_DOWN;
    kernel_queue[queue_tail - 1].b = 12;

    struct wm_event wm_event;
    expect(wm_poll_event(&wm_event) == 1, "WM event found behind custom IPC");
    expect(wm_event.type == WM_EV_KEY_DOWN && wm_event.param == 12,
           "WM event translated");
    expect(event_pending() == 1, "custom IPC was deferred, not dropped");

    struct ipc_msg custom;
    expect(event_poll(&custom) == EVENT_READY && custom.type == custom_type,
           "deferred custom IPC retains order");

    ticks = 0;
    struct ipc_msg missing;
    expect(event_wait_type(&missing, 0xdead, 0, 3) == EVENT_TIMEOUT,
           "bounded event wait times out");
    expect(ticks == 3, "event timeout uses kernel ticks");

    /* Fill the deferred inbox, then put the desired event behind it. The
     * full result must leave that kernel message untouched for a retry. */
    for (int i = 0; i < EVENT_INBOX_CAPACITY; i++)
        queue_message(custom_type + (uint32_t)i, 77);
    queue_message(IPC_WM_INPUT, 42);
    kernel_queue[queue_tail - 1].a = WM_EV_MOUSE_MOVE;
    expect(wm_poll_event(&wm_event) == 0 &&
           event_pending() == EVENT_INBOX_CAPACITY,
           "filtered polling fills its inbox without dropping messages");
    expect(wm_poll_event(&wm_event) == 0,
           "full inbox does not consume the waiting kernel event");
    expect(event_poll(&custom) == EVENT_READY,
           "caller can make room in a full inbox");
    expect(wm_poll_event(&wm_event) == 1 &&
           wm_event.type == WM_EV_MOUSE_MOVE,
           "desired kernel event remains available after making room");
    while (event_poll(&custom) == EVENT_READY)
        ;
}

static void test_app_lifecycle(void) {
    struct app application;
    last_sent_type = 0;
    expect(app_open(&application, 80, 60, "test") == 0,
           "app creates a window");
    expect(application.open && application.window.handle == 7,
           "app stores the WM handle");
    expect(application.surface.px == fake_surface &&
           application.surface.w == 80 && application.surface.stride == 80,
           "app binds the shared surface");

    queue_message(IPC_WM_RESIZE_NOTIFY, 42);
    struct ipc_msg *resize = &kernel_queue[queue_tail - 1];
    resize->b = 40;
    resize->c = 30;
    resize->va = (uint64_t)(uintptr_t)fake_surface;
    resize->pitch = 80 * 4;

    struct wm_event event;
    expect(app_poll_event(&application, &event) == 1 &&
           event.type == WM_EV_RESIZE, "app receives resize");
    expect(application.window.w == 40 && application.surface.w == 40 &&
           application.surface.stride == 80,
           "app automatically rebinds resized surface and pitch");

    expect(app_present(&application) == 0 &&
           last_sent_type == IPC_WM_INVALIDATE_REQ,
           "app present invalidates its window");
    app_close(&application);
    expect(!application.open && last_sent_type == IPC_WM_DESTROY_REQ,
           "app close is idempotent lifecycle cleanup");
}

static void test_process_utilities(void) {
    char path[PROCESS_PATH_MAX];
    expect(process_resolve("hello", "/bin:/usr/bin", path, sizeof(path)) == 0 &&
           strcmp(path, "/usr/bin/hello.elf") == 0,
           "process lookup searches PATH and appends .elf");
    expect(process_resolve("tool.exe", "/system/bin", path, sizeof(path)) == 0 &&
           strcmp(path, "/system/bin/tool.exe") == 0,
           "explicit extension is preserved");
    expect(process_resolve("relative/app", 0, path, sizeof(path)) == 0 &&
           strcmp(path, "relative/app.elf") == 0,
           "paths containing separators bypass PATH");
    expect(process_resolve("missing", 0, path, sizeof(path)) != 0,
           "missing executable fails cleanly");
    expect(process_resolve("folder", "/usr/bin", path, sizeof(path)) != 0,
           "process lookup rejects directories");

    char *argv[] = {(char *)"hello", 0};
    expect(process_spawn("hello", argv, "/usr/bin") == 123 &&
           strcmp(last_launched, "/usr/bin/hello.elf") == 0,
           "process spawn launches resolved path");

    process_table_mode = TABLE_RUNNING;
    ticks = 0;
    expect(process_is_alive(123) == 1, "running process is alive");
    expect(process_wait(123, 2, 0) == PROCESS_TIMEOUT && ticks == 2,
           "process wait has a bounded timeout");
    process_table_mode = TABLE_ZOMBIE;
    expect(process_wait(123, 0, 0) == PROCESS_EXITED,
           "zombie process counts as exited");
    process_table_mode = TABLE_ABSENT;
    expect(process_wait(123, 0, 0) == PROCESS_EXITED,
           "disappeared process counts as exited");
}

int main(void) {
    test_event_filtering();
    test_app_lifecycle();
    test_process_utilities();
    if (!failed)
        printf("userspace_runtime_test: all checks passed\n");
    return failed;
}
