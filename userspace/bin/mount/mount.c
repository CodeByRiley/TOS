/* userspace/bin/mount/mount.c , list storage volumes and mount them.
 *
 * usage: mount                          list the volumes the drivers found
 *        mount <source> [type]          mount at /mnt/<source>
 *        mount <source> <target> [type] mount one at a chosen path
 *        mount -u <target>              unmount
 *
 * `source` is a volume name from the listing ("ahci0"), with an optional
 * "/dev/" prefix. Leaving `type` off lets the kernel probe for the format.
 *
 * There is no mount table syscall yet, so the listing says what exists, not
 * what is mounted. `ls` a mountpoint to see whether a mount took.
 */
#include <errno.h>
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

static const char *disk_err(int errnum) {
  switch (errnum) {
  case ENOENT:
    return "no such disk";
  case ENODEV:
    return "unsupported filesystem";
  case EACCES:
    return "permission denied";
  case EBUSY:
    return "disk in use";
  case EINVAL:
    return "invalid disk";
  case ENOSYS:
    return "not implemented";
  default:
    return strerror(errnum);
  }
}

/* The filesystem names currently mountable from a block device.  Keeping
 * this list narrow preserves `mount ahci1 relative-target` for existing
 * callers of the explicit-target form. */
static int filesystem_name(const char *name) {
  return !strcmp(name, "fat") || !strcmp(name, "ext2");
}

/* A device name makes a useful default mountpoint.  /dev/ is accepted in the
 * source spelling but is not copied into the pathname. */
static int default_target(const char *source,
                          char out[sizeof("/mnt/") + BLOCKDEV_NAME_MAX]) {
  const char *name = !strncmp(source, "/dev/", 5) ? source + 5 : source;
  size_t length = strlen(name);
  if (length == 0 || length >= BLOCKDEV_NAME_MAX)
    return -1;
  memcpy(out, "/mnt/", sizeof("/mnt/") - 1);
  memcpy(out + sizeof("/mnt/") - 1, name, length + 1);
  return 0;
}

static void usage(void) {
  printf("usage: mount\n");
  printf("       mount <source> [type]\n");
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
      int errnum = errno;

      printf("mount: cannot unmount %s: %s\n", argv[2], disk_err(errnum));
      return 1;
    }
    return 0;
  }

  if (argc < 2 || argc > 4) {
    usage();
    return 2;
  }

  char inferred_target[sizeof("/mnt/") + BLOCKDEV_NAME_MAX];
  const char *target = NULL;
  const char *type = NULL;
  if (argc == 4) {
    target = argv[2];
    type = argv[3];
  } else if (argc == 3 && !filesystem_name(argv[2])) {
    target = argv[2];
  } else {
    type = argc == 3 ? argv[2] : NULL;
    if (default_target(argv[1], inferred_target) != 0) {
      printf("mount: invalid disk name %s\n", argv[1]);
      return 2;
    }
    target = inferred_target;
  }

  if (mount(argv[1], target, type, 0, NULL) != 0) {
    int errnum = errno;

    if (errnum == EINVAL && type && *type) {
      printf("mount: %s is not a %s filesystem\n", argv[1], type);
    } else if (errnum == EINVAL) {
      printf("mount: no supported filesystem found on %s\n", argv[1]);
      printf("mount: if %s is a whole disk, try one of its partitions "
             "(mount with no arguments lists them)\n",
             argv[1]);
    } else {
      printf("mount: cannot mount %s at %s: %s\n", argv[1], target,
             disk_err(errnum));
    }

    return 1;
  }
  return 0;
}
