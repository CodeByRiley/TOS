/* userspace/bin/lsdisk/lsdisk.c , list block volumes published by drivers.
 *
 * The names are the sources accepted by mount(8): `/dev/ahci0`, `/dev/usb0`,
 * and so on.  The kernel supplies sectors rather than a formatted capacity,
 * so this command keeps the exact count visible and adds a convenient MiB
 * column for people at the console.
 */
#include <lib/syscall.h>
#include <stdio.h>

#define MAX_VOLUMES 64
#define MIB (1024ULL * 1024ULL)

static void usage(void) { printf("usage: lsdisk\n"); }

int main(int argc, char **argv) {
  (void)argv;
  static struct blockdev_info volumes[MAX_VOLUMES];

  if (argc != 1) {
    usage();
    return 2;
  }

  long count = blockdev_list(volumes, MAX_VOLUMES);
  if (count < 0) {
    printf("lsdisk: could not read the disk list\n");
    return 1;
  }
  if (count == 0) {
    printf("lsdisk: no disks found\n");
    return 0;
  }

  printf("%-20s %-14s %-10s %-12s %s\n", "DEVICE", "SECTORS", "SECTOR", "SIZE",
         "ACCESS");
  for (long i = 0; i < count; i++) {
    struct blockdev_info *volume = &volumes[i];

    /* This data crossed the syscall boundary; do not let a malformed
     * driver name make printf walk past the ABI's fixed-size field. */
    volume->name[BLOCKDEV_NAME_MAX - 1] = '\0';

    /* Divide by sectors-per-MiB instead of first forming a byte total;
     * that keeps capacity formatting safe for very large disks. */
    unsigned long long mib = 0;
    if (volume->sector_size && volume->sector_size <= MIB)
      mib = (unsigned long long)(volume->sectors / (MIB / volume->sector_size));
    printf("/dev/%-15s %-14llu %-10u %-8llu MiB %s\n", volume->name,
           (unsigned long long)volume->sectors, volume->sector_size, mib,
           volume->flags & BLOCKDEV_WRITABLE ? "rw" : "ro");
  }
  return 0;
}
