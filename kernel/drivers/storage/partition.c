/* MBR parsing and the slice devices it publishes. See partition.h.
 *
 * The table lives in the last 66 bytes of LBA 0: four 16-byte entries at
 * offset 446 followed by 0x55 0xAA. Everything before it is boot code this
 * kernel never runs.
 *
 * Primary entries are numbered 1..4 by slot, so an empty slot 2 leaves a gap
 * rather than renumbering the disk -- the number is an address, not a count.
 * Logical partitions inside an extended one continue from 5, in chain order,
 * which is the same numbering Linux and DOS produce for the same disk.
 */
#include <drivers/storage/blockdev.h>
#include <drivers/storage/partition.h>
#include <utilities/log.h>
#include <utilities/string.h>

#define MBR_TABLE_OFFSET 446u
#define MBR_ENTRY_BYTES 16u
#define MBR_ENTRIES 4u
#define MBR_SIGNATURE_OFFSET 510u

#define MBR_STATUS_INACTIVE 0x00u
#define MBR_STATUS_BOOTABLE 0x80u

#define MBR_TYPE_EMPTY 0x00u
#define MBR_TYPE_EXTENDED_CHS 0x05u
#define MBR_TYPE_EXTENDED_LBA 0x0Fu
#define MBR_TYPE_EXTENDED_LINUX 0x85u
#define MBR_TYPE_GPT_PROTECTIVE 0xEEu

/* A malformed or hostile EBR chain must not spin forever. Real disks use a
 * handful of links; DOS itself stopped well before this. */
#define EBR_MAX_LINKS 64u

#define LOGICAL_FIRST_NUMBER 5u

struct partition_device {
    struct block_device device; /* Published; .context is this record. */
    const struct block_device *parent;
    u64 first; /* Parent LBA holding this slice's sector 0. */
};

/* Static because a mount borrows the transport for as long as it is mounted,
 * and nothing here is ever released. */
static struct partition_device slices[PARTITION_MAX];
static usize slice_count;

static int slice_read(void *context, u64 lba, u32 count, void *buffer) {
    const struct partition_device *slice = context;
    return block_read(slice->parent, slice->first + lba, count, buffer);
}

static int slice_write(void *context, u64 lba, u32 count, const void *buffer) {
    const struct partition_device *slice = context;
    return block_write(slice->parent, slice->first + lba, count, buffer);
}

/* Flushing a slice flushes the disk: the write cache belongs to the drive,
 * not to any one partition on it. */
static int slice_flush(void *context) {
    const struct partition_device *slice = context;
    return block_flush(slice->parent);
}

/* MBR fields are little-endian and the table is only 2-byte aligned, so read
 * them a byte at a time rather than casting. */
static u32 read_le32(const u8 *at) {
    return (u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16) |
           ((u32)at[3] << 24);
}

static int is_extended(u8 type) {
    return type == MBR_TYPE_EXTENDED_CHS || type == MBR_TYPE_EXTENDED_LBA ||
           type == MBR_TYPE_EXTENDED_LINUX;
}

/* "<parent>p<number>", or -1 if that does not fit a published name. */
static int slice_name(char *out, usize max, const char *parent, u32 number) {
    usize at = 0;
    while (parent[at]) {
        if (at + 1 >= max)
            return -1;
        out[at] = parent[at];
        at++;
    }
    char digits[8];
    usize count = 0;
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number && count < sizeof(digits));
    if (at + count + 2 > max)
        return -1;
    out[at++] = 'p';
    while (count)
        out[at++] = digits[--count];
    out[at] = 0;
    return 0;
}

static int has_signature(const u8 *sector) {
    return sector[MBR_SIGNATURE_OFFSET] == 0x55 &&
           sector[MBR_SIGNATURE_OFFSET + 1] == 0xAA;
}

/* Publish one slice. Returns 1 when it was published, 0 when it was rejected
 * or could not be named -- a bad entry skips its slot, it does not abort the
 * disk, because the other entries may still be sound. */
static int publish(const struct block_device *parent, const char *parent_name,
                   u32 number, u64 first, u64 sectors, u32 inherit) {
    if (!sectors || first >= parent->sectors ||
        sectors > parent->sectors - first)
        return 0;
    if (slice_count >= PARTITION_MAX) {
        log_write("partition: slice table full", KERNEL, LOG_WARN);
        return 0;
    }

    char name[BLOCKDEV_NAME_MAX];
    if (slice_name(name, sizeof(name), parent_name, number) != 0)
        return 0;

    struct partition_device *slice = &slices[slice_count];
    slice->parent = parent;
    slice->first = first;
    slice->device = (struct block_device){
        .context = slice,
        .sectors = sectors,
        .read = slice_read,
        /* A partition is only writable if the disk under it is. */
        .write = parent->write ? slice_write : 0,
        .flush = parent->flush ? slice_flush : 0,
    };

    if (blockdev_register(name, &slice->device, first,
                          inherit | BLOCKDEV_PARTITION) != 0)
        return 0;
    slice_count++;
    log_write_fmt(KERNEL, LOG_INFO,
                  "partition: %s at LBA %llu, %llu sectors", name,
                  (unsigned long long)first, (unsigned long long)sectors);
    return 1;
}

/* Walk the EBR chain of an extended partition. Each EBR's first entry is a
 * logical partition relative to that EBR, and its second entry, when present,
 * points at the next EBR relative to the extended partition's own start --
 * two different bases, which is the classic way to misread this table. */
static int scan_logical(const struct block_device *parent,
                        const char *parent_name, u64 extended_first,
                        u32 inherit) {
    int published = 0;
    u32 number = LOGICAL_FIRST_NUMBER;
    u64 here = extended_first;

    for (u32 link = 0; link < EBR_MAX_LINKS; link++) {
        u8 sector[BLOCK_SECTOR_SIZE];
        if (here >= parent->sectors ||
            block_read(parent, here, 1, sector) != 0 || !has_signature(sector))
            break;

        const u8 *entry = sector + MBR_TABLE_OFFSET;
        u64 first = read_le32(entry + 8);
        u64 sectors = read_le32(entry + 12);
        if (entry[4] != MBR_TYPE_EMPTY && sectors)
            published += publish(parent, parent_name, number, here + first,
                                 sectors, inherit);
        number++;

        const u8 *next = entry + MBR_ENTRY_BYTES;
        if (!is_extended(next[4]))
            break;
        u64 offset = read_le32(next + 8);
        if (!offset)
            break;
        u64 following = extended_first + offset;
        /* The chain must move forward. A link that points at or behind this
         * EBR is a loop, and the counter above would only bound it. */
        if (following <= here)
            break;
        here = following;
    }
    return published;
}

int partition_scan(const struct block_device *parent, const char *parent_name,
                   uint32_t inherit) {
    u8 sector[BLOCK_SECTOR_SIZE];
    if (!parent || !parent_name ||
        block_read(parent, 0, 1, sector) != 0 || !has_signature(sector))
        return 0;

    /* Validate the whole table before publishing any of it. A FAT boot sector
     * also ends in 0x55 0xAA, and its BPB lands where the table does, so the
     * signature alone does not mean a partition table is present: require
     * every status byte to be one of the two legal values and at least one
     * entry to describe a plausible region. */
    int usable = 0;
    for (u32 i = 0; i < MBR_ENTRIES; i++) {
        const u8 *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        if (entry[0] != MBR_STATUS_INACTIVE && entry[0] != MBR_STATUS_BOOTABLE)
            return 0;
        if (entry[4] == MBR_TYPE_GPT_PROTECTIVE) {
            log_write_string("partition: GPT is not supported on", parent_name,
                             KERNEL, LOG_WARN);
            return 0;
        }
        u64 first = read_le32(entry + 8);
        u64 sectors = read_le32(entry + 12);
        if (entry[4] == MBR_TYPE_EMPTY || !sectors)
            continue;
        if (first < parent->sectors && sectors <= parent->sectors - first)
            usable = 1;
    }
    if (!usable)
        return 0;

    int published = 0;
    for (u32 i = 0; i < MBR_ENTRIES; i++) {
        const u8 *entry = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        u64 first = read_le32(entry + 8);
        u64 sectors = read_le32(entry + 12);
        if (entry[4] == MBR_TYPE_EMPTY || !sectors)
            continue;
        if (is_extended(entry[4])) {
            /* The extended partition is a container, not a volume: only the
             * logical partitions inside it hold filesystems. */
            published += scan_logical(parent, parent_name, first, inherit);
            continue;
        }
        published += publish(parent, parent_name, i + 1, first, sectors,
                             inherit);
    }
    return published;
}
