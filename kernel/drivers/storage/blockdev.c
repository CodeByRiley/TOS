/* Publication and name lookup for block volumes. See blockdev.h. */
#include <drivers/storage/blockdev.h>
#include <sync/spinlock.h>
#include <utilities/string.h>

struct blockdev_entry {
    char name[BLOCKDEV_NAME_MAX];
    struct block_device device;
    uint64_t start_lba;
    uint32_t flags;
};

static struct blockdev_entry entries[BLOCKDEV_MAX];
static size_t entry_count;
static struct spinlock lock = SPINLOCK_INIT;

/* Callers hold the lock. */
static struct blockdev_entry *find(const char *name) {
    for (size_t i = 0; i < entry_count; i++)
        if (!strcmp(entries[i].name, name))
            return &entries[i];
    return 0;
}

int blockdev_register(const char *name, const struct block_device *device,
                      uint64_t start_lba, uint32_t flags) {
    if (!name || !*name || strlen(name) >= BLOCKDEV_NAME_MAX || !device ||
        !device->read || !device->sectors)
        return -1;

    int result = -1;
    spin_lock(&lock);
    if (entry_count < BLOCKDEV_MAX && !find(name)) {
        struct blockdev_entry *entry = &entries[entry_count];
        strcpy(entry->name, name);
        entry->device = *device;
        entry->start_lba = start_lba;
        entry->flags = flags | (device->write ? BLOCKDEV_WRITABLE : 0);
        entry_count++;
        result = 0;
    }
    spin_unlock(&lock);
    return result;
}

int blockdev_set_flags(const char *name, uint32_t flags) {
    if (!name)
        return -1;

    int result = -1;
    spin_lock(&lock);
    struct blockdev_entry *entry = find(name);
    if (entry) {
        entry->flags |= flags;
        result = 0;
    }
    spin_unlock(&lock);
    return result;
}

size_t blockdev_count(void) {
    spin_lock(&lock);
    size_t count = entry_count;
    spin_unlock(&lock);
    return count;
}

int blockdev_describe(size_t index, struct blockdev_info *out) {
    if (!out)
        return -1;

    int result = -1;
    spin_lock(&lock);
    if (index < entry_count) {
        const struct blockdev_entry *entry = &entries[index];
        memset(out, 0, sizeof(*out));
        out->sectors = entry->device.sectors;
        out->start_lba = entry->start_lba;
        out->sector_size = BLOCK_SECTOR_SIZE;
        out->flags = entry->flags;
        strcpy(out->name, entry->name);
        result = 0;
    }
    spin_unlock(&lock);
    return result;
}

int blockdev_lookup(const char *name, struct block_device *out) {
    if (!name || !out)
        return -1;

    int result = -1;
    spin_lock(&lock);
    const struct blockdev_entry *entry = find(name);
    if (entry) {
        *out = entry->device;
        result = 0;
    }
    spin_unlock(&lock);
    return result;
}
