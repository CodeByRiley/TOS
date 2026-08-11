/* Host-side regression test for the kernel FILE* layer in fs/stdio.c.
 *
 * Covers the parts that are easy to get subtly wrong and impossible to
 * see from a boot: mode-string parsing, whether "w" can destroy a file it
 * then fails to recreate, append forcing writes to EOF, and fread/fwrite
 * returning complete items rather than bytes (the ELF and PE loaders test
 * the result against 1).
 *
 * Build:
 *   gcc -I kernel -o stdio_test tests/stdio_mode_test.c \
 *       kernel/fs/stdio.c kernel/fs/fat.c
 */
#include "fs/stdio.h"
#include "fs/fat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Host <stdio.h> cannot be included: its FILE, fopen, fread and friends
 * collide with the kernel ones under test. printf touches neither. */
extern int printf(const char *fmt, ...);

#define SECTOR_SIZE 512
#define TOTAL_SECTORS 6000
#define FAT_SECTORS 24
#define ROOT_ENTRIES 32

struct __attribute__((packed)) test_bpb {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t large_total_sectors;
};

/* fat.c only logs geometry during initialization. */
void log_write_hex(const char *message, uint64_t value,
                   uint8_t type, uint8_t level) {
    (void)message; (void)value; (void)type; (void)level;
}

void log_write(const char *message, uint8_t type, uint8_t level) {
    (void)message; (void)type; (void)level;
}

/* stdio.c allocates one FILE per open. */
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p)    { free(p); }

static int failed = 0;

static void expect(int condition, const char *message) {
    if (condition) return;
    printf("FAIL: %s\n", message);
    failed = 1;
}

static void write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (!fp) { expect(0, "setup: fopen for write"); return; }
    fwrite(text, 1, strlen(text), fp);
    fclose(fp);
}

static long file_size(const char *path) {
    struct fat_stat st;
    if (fat_stat(path, &st) != 0) return -1;
    return (long)st.size;
}

int main(void) {
    size_t image_size = (size_t)TOTAL_SECTORS * SECTOR_SIZE;
    uint8_t *image = calloc(1, image_size);
    if (!image) return 1;

    struct test_bpb *bpb = (struct test_bpb *)image;
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 1;
    bpb->fat_count = 1;
    bpb->root_entries = ROOT_ENTRIES;
    bpb->total_sectors = TOTAL_SECTORS;
    bpb->media = 0xF8;
    bpb->sectors_per_fat = FAT_SECTORS;

    uint16_t *fat = (uint16_t *)(image + SECTOR_SIZE);
    fat[0] = 0xFFF8;
    fat[1] = 0xFFFF;

    expect(fat_init(image, image_size) == 0, "initialize synthetic image");

    /* --- mode strings --------------------------------------------------- */
    expect(fopen("/MISSING.TXT", "r") == 0, "\"r\" on a missing file fails");
    expect(fopen("/MISSING.TXT", "r+") == 0,
           "\"r+\" does not create a missing file");
    expect(fopen("/ANY.TXT", "q") == 0, "unknown base mode rejected");
    expect(fopen("/ANY.TXT", "rq") == 0, "unknown modifier rejected");

    write_file("/KEEP.TXT", "original contents");
    expect(file_size("/KEEP.TXT") == 17, "setup wrote 17 bytes");

    /* "rw" is the classic typo for "r+". It must not be read as
     * read-plus-truncate. */
    expect(fopen("/KEEP.TXT", "rw") == 0, "\"rw\" is rejected outright");
    expect(file_size("/KEEP.TXT") == 17, "\"rw\" left the file intact");

    FILE *fp = fopen("/KEEP.TXT", "r+");
    expect(fp != 0, "\"r+\" opens an existing file");
    expect(file_size("/KEEP.TXT") == 17, "\"r+\" did not truncate");
    if (fp) fclose(fp);

    /* --- item counts, not bytes ---------------------------------------- */
    struct { uint32_t a; uint32_t b; uint64_t c; } recs[4], back[4];
    for (int i = 0; i < 4; i++) {
        recs[i].a = (uint32_t)i;
        recs[i].b = 0xAAAA0000u | (uint32_t)i;
        recs[i].c = 0x1122334455667788ull + (uint64_t)i;
    }

    fp = fopen("/RECS.BIN", "w");
    expect(fp != 0, "open /RECS.BIN for write");
    if (fp) {
        expect(fwrite(recs, sizeof(recs[0]), 4, fp) == 4,
               "fwrite returns item count");
        fclose(fp);
    }
    expect(file_size("/RECS.BIN") == (long)sizeof(recs),
           "file holds every byte written");

    fp = fopen("/RECS.BIN", "r");
    expect(fp != 0, "reopen /RECS.BIN");
    if (fp) {
        expect(fread(back, sizeof(back[0]), 4, fp) == 4,
               "fread returns item count");
        expect(memcmp(recs, back, sizeof(recs)) == 0, "contents round-trip");
        fclose(fp);
    }

    /* A header read is the shape both loaders use: one item of sizeof. */
    fp = fopen("/RECS.BIN", "r");
    if (fp) {
        expect(fread(back, sizeof(recs), 1, fp) == 1,
               "fread(&hdr, sizeof hdr, 1, fp) == 1");
        fclose(fp);
    }

    /* Asking for more items than remain yields the complete ones only. */
    fp = fopen("/RECS.BIN", "r");
    if (fp) {
        expect(fread(back, sizeof(back[0]) * 3, 4, fp) == 1,
               "short read floors to complete items");
        fclose(fp);
    }

    /* --- degenerate arguments ------------------------------------------- */
    fp = fopen("/RECS.BIN", "r");
    if (fp) {
        expect(fread(back, 0, 4, fp) == 0, "size 0 is not a divide by zero");
        expect(fread(back, 4, 0, fp) == 0, "count 0 reads nothing");
        expect(fread(back, (size_t)-1, 2, fp) == 0,
               "size * count overflow rejected");
        fclose(fp);
    }

    /* --- access rights --------------------------------------------------- */
    fp = fopen("/WONLY.TXT", "w");
    if (fp) {
        expect(fread(back, 1, 4, fp) == 0, "fread on a write-only handle");
        fclose(fp);
    }
    fp = fopen("/KEEP.TXT", "r");
    if (fp) {
        expect(fwrite("x", 1, 1, fp) == 0, "fwrite on a read-only handle");
        fclose(fp);
    }
    expect(file_size("/KEEP.TXT") == 17, "read-only handle wrote nothing");

    /* --- truncate keeps the file ---------------------------------------- */
    fp = fopen("/KEEP.TXT", "w");
    expect(fp != 0, "\"w\" reopens an existing file");
    expect(file_size("/KEEP.TXT") == 0, "\"w\" truncated in place");
    expect(fat_stat("/KEEP.TXT", &(struct fat_stat){0}) == 0,
           "\"w\" kept the directory entry");
    if (fp) {
        expect(fwrite("new", 1, 3, fp) == 3, "write after truncate");
        fclose(fp);
    }
    expect(file_size("/KEEP.TXT") == 3, "truncated file holds new contents");

    /* --- append forces writes to EOF ------------------------------------ */
    write_file("/LOG.TXT", "AAAA");
    fp = fopen("/LOG.TXT", "a");
    expect(fp != 0, "open for append");
    if (fp) {
        expect(ftell(fp) == 4, "append starts at EOF");
        /* Seeking away must not change where the data lands. */
        fseek(fp, 0, SEEK_SET);
        expect(fwrite("BB", 1, 2, fp) == 2, "append write");
        fclose(fp);
    }
    expect(file_size("/LOG.TXT") == 6, "append grew the file");

    char tail[8] = {0};
    fp = fopen("/LOG.TXT", "r");
    if (fp) {
        fread(tail, 1, 6, fp);
        fclose(fp);
    }
    expect(memcmp(tail, "AAAABB", 6) == 0,
           "append landed at EOF despite the seek");

    /* --- append creates when missing ------------------------------------ */
    fp = fopen("/NEWLOG.TXT", "a");
    expect(fp != 0, "\"a\" creates a missing file");
    if (fp) {
        expect(fwrite("hi", 1, 2, fp) == 2, "write to created file");
        fclose(fp);
    }
    expect(file_size("/NEWLOG.TXT") == 2, "created file has contents");

    free(image);
    if (!failed) printf("stdio_mode_test: all checks passed\n");
    return failed;
}
