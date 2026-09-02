/* Real VFS/ext2 operations and the real sleeping FIFO; only IRQ/scheduling
 * are hosted by pthreads. Run via make test-fs (includes panic checks). */
#include <fs/ext2/ext2.h>
#include <fs/vfs/vfs.h>
#include <fs/vfs/lock.h>
#include <sched/sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include "vfs_lock_host.h"
#include <string.h>

enum { WORKERS = 8, ROUNDS = 40 };
static struct vfs_file shared;
static atomic_uint winners;
struct worker { unsigned id; struct vfs_file append; };

static void *mutate(void *context) {
    struct worker *worker = context;
    char dir[32], path[64];
    snprintf(dir, sizeof(dir), "/worker%u", worker->id);
    assert(!vfs_mkdir(dir));
    struct vfs_file file;
    if (!vfs_create("/winner", &file)) {
        atomic_fetch_add(&winners, 1);
        vfs_close(&file);
    }
    for (unsigned i = 0; i < ROUNDS; i++) {
        uint32_t packet[2] = { worker->id, i }, back[2] = {0};
        snprintf(path, sizeof(path), "%s/item%u", dir, i);
        assert(!vfs_create(path, &file));
        assert(vfs_write(&file, packet, sizeof(packet)) == sizeof(packet));
        assert(!vfs_seek(&file, 0));
        assert(vfs_read(&file, back, sizeof(back)) == sizeof(back));
        assert(!memcmp(packet, back, sizeof(back)));
        struct vfs_stat metadata;
        assert(!vfs_stat(path, &metadata) && metadata.size == sizeof(packet));
        uint32_t index = 0;
        struct vfs_dirent entry;
        assert(vfs_read_dir_one(dir, &index, &entry) > 0);
        assert(vfs_unlink(path) == -1); /* A live handle still pins the inode. */
        assert(!vfs_truncate(&file));
        vfs_close(&file);
        assert(!vfs_unlink(path));
        assert(vfs_append(&worker->append, packet, sizeof(packet)) == sizeof(packet));
        assert(vfs_write(&shared, packet, sizeof(packet)) == sizeof(packet));
    }
    vfs_close(&worker->append);
    assert(!vfs_rmdir(dir));
    assert(!task_current()->vfs_active);
    return 0;
}

static void verify_packets(struct vfs_file *file) {
    unsigned seen[WORKERS][ROUNDS] = {{0}};
    struct vfs_stat metadata;
    assert(!vfs_file_stat(file, &metadata));
    assert(metadata.size == WORKERS * ROUNDS * 2 * sizeof(uint32_t));
    assert(!vfs_seek(file, 0));
    for (unsigned i = 0; i < WORKERS * ROUNDS; i++) {
        uint32_t packet[2];
        assert(vfs_read(file, packet, sizeof(packet)) == sizeof(packet));
        assert(packet[0] < WORKERS && packet[1] < ROUNDS);
        assert(!seen[packet[0]][packet[1]]++);
    }
}

/* An owner simulates sleeping I/O. Readers/close/unmount must park in FIFO
 * order; closing cannot free the reader's backend state before it returns. */
static atomic_uint order;
static void *queued_operation(void *context) {
    unsigned operation = *(unsigned *)context;
    if (operation == 0) {
        uint32_t packet[2];
        assert(vfs_read(&shared, packet, sizeof(packet)) == sizeof(packet));
    } else if (operation == 1) vfs_close(&shared);
    else assert(!vfs_unmount("/"));
    /* The read requires an open handle, and unmount requires a closed one.
     * Completion after unlock need not follow the operations' FIFO order. */
    atomic_fetch_add(&order, 1);
    assert(!task_current()->vfs_active);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "recursive")) { vfs_lock(); vfs_sync_all(); }
        else if (!strcmp(argv[1], "helper")) vfs_inode_put(0);
        else if (!strcmp(argv[1], "unlock")) vfs_unlock();
        else if (!strcmp(argv[1], "irq")) { vfs_test_context(0, 1, 0); vfs_sync_all(); }
        else if (!strcmp(argv[1], "ap")) { vfs_test_context(1, 0, 0); vfs_sync_all(); }
        return 1; /* Each invalid entry must exit with panic status 86. */
    }
    vfs_test_context(0, 0, 1);
    vfs_init(); /* Bootstrap has no current task yet. */
    ext2_vfs_register();
    vfs_test_context(0, 0, 0);
    FILE *input = fopen("build/tests/ext2-base.img", "rb");
    assert(input);
    assert(!fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0);
    rewind(input);
    void *image = malloc((size_t)size);
    assert(image && fread(image, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    assert(!vfs_mount_image("/", "ext2", image, (size_t)size));
    assert(!vfs_create("/shared", &shared));
    struct vfs_file append;
    assert(!vfs_create("/append", &append));
    pthread_t threads[WORKERS];
    struct worker workers[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        workers[i].id = i;
        assert(!vfs_open("/append", &workers[i].append));
    }
    for (unsigned i = 0; i < WORKERS; i++)
        assert(!pthread_create(&threads[i], 0, mutate, &workers[i]));
    for (unsigned i = 0; i < WORKERS; i++) assert(!pthread_join(threads[i], 0));
    assert(atomic_load(&winners) == 1);
    verify_packets(&shared);
    verify_packets(&append);
    vfs_close(&append);
    assert(vfs_unmount("/") == -1); /* Error paths release the gate. */
    assert(!vfs_seek(&shared, 0));

    vfs_lock();
    assert(task_current()->vfs_active);
    unsigned operations[] = {0, 1, 2};
    for (unsigned i = 0; i < 3; i++) {
        assert(!pthread_create(&threads[i], 0, queued_operation, &operations[i]));
        vfs_test_wait_parked(i + 1);
    }
    assert(!atomic_load(&order));
    vfs_unlock();
    for (unsigned i = 0; i < 3; i++) assert(!pthread_join(threads[i], 0));
    assert(atomic_load(&order) == 3 && !shared.node);
    assert(!task_current()->vfs_active);
    /* A fresh mount catches leaked references or a half-published unmount. */
    assert(!vfs_mount_image("/", "ext2", image, (size_t)size));
    assert(!vfs_open("/append", &append));
    verify_packets(&append);
    vfs_close(&append);
    assert(!vfs_unmount("/"));
    free(image);
    puts("vfs_serialization_test: contention, lifetime, append and FIFO checks passed");
    return 0;
}
