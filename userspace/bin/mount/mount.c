/* userspace/bin/mount/mount.c , list storage volumes and mount them.
 *
 * usage: mount                          list the volumes the drivers found
 *        mount <source> <target> [type] mount one at a path
 *        mount -u <target>              unmount
 *
 * `source` is a volume name from the listing ("ahci0"), with an optional
 * "/dev/" prefix. Leaving `type` off lets the kernel probe for the format.
 *
 * There is no mount table syscall yet, so the listing says what exists, not
 * what is mounted. `ls` a mountpoint to see whether a mount took.
 */
#include <lib/syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#define MAX_VOLUMES 64
#define MIB (1024ULL * 1024ULL)

static int list_volumes(void) {
  static struct blockdev_info volumes[MAX_VOLUMES];

  long count = blockdev_list(volumes, MAX_VOLUMES);
  if (count < 0) {
    printf("mount: could not read the volume list\n");
    return 1;
  }
  if (count == 0) {
    printf("mount: no storage volumes\n");
    return 0;
  }

  printf("%-10s %-12s %-14s %-10s %s\n", "VOLUME", "SIZE", "START", "KIND",
         "ACCESS");
  for (long i = 0; i < count; i++) {
    struct blockdev_info *volume = &volumes[i];

    /* The name crossed a syscall boundary; bound it before printing. */
    volume->name[BLOCKDEV_NAME_MAX - 1] = '\0';

    /* Divide by sectors-per-MiB rather than forming a byte total first, so a
     * very large disk cannot overflow on the way to a readable number. */
    unsigned long long mib = 0;
    if (volume->sector_size && volume->sector_size <= MIB)
      mib = (unsigned long long)(volume->sectors / (MIB / volume->sector_size));

    /* A partitioned disk is listed but is not itself mountable: its table sits
     * at sector 0 where a filesystem would be. Say so, rather than leave
     * someone to work out why mounting it always fails. */
    const char *kind = volume->flags & BLOCKDEV_PARTITIONED ? "table"
                       : volume->flags & BLOCKDEV_PARTITION ? "partition"
                                                            : "disk";
    printf("%-10s %-8llu MiB %-14llu %-10s %s\n", volume->name, mib,
           (unsigned long long)volume->start_lba, kind,
           volume->flags & BLOCKDEV_WRITABLE ? "rw" : "ro");
  }
  return 0;
}

static void usage(void) {
  printf("usage: mount\n");
  printf("       mount <source> <target> [type]\n");
  printf("       mount -u <target>\n");
}

int main(int argc, char **argv) {
  if (argc == 1)
    return list_volumes();

  if (!strcmp(argv[1], "-u")) {
    if (argc != 3) {
      usage();
      return 2;
    }
    if (umount(argv[2]) != 0) {
      printf("mount: could not unmount %s\n", argv[2]);
      return 1;
    }
    return 0;
  }

  if (argc < 3 || argc > 4) {
    usage();
    return 2;
  }

  const char *type = argc == 4 ? argv[3] : NULL;
  if (mount(argv[1], argv[2], type, 0, NULL) != 0) {
    printf("mount: could not mount %s at %s\n", argv[1], argv[2]);
    return 1;
  }
  return 0;
}
