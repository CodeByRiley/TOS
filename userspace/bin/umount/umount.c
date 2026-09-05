/* userspace/bin/umount/umount.c -- detach a filesystem from a path. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

static const char *mount_err(int errnum) {
  switch (errnum) {
  case EINVAL:
    return "not a mountpoint";
  case EBUSY:
    return "mountpoint or volume is busy";
  case EACCES:
    return "permission denied";
  case ENOSYS:
    return "not implemented";
  default:
    return strerror(errnum);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("usage: umount <target>\n");
    return 2;
  }

  if (umount(argv[1]) == 0)
    return 0;

  printf("umount: cannot unmount %s: %s\n", argv[1], mount_err(errno));
  return 1;
}
