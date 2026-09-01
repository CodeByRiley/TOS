#include "utilities/log.h"
#include <fs/vfs/vfs.h>
#include <utilities/string.h>

struct vfs_mount {
  char path[VFS_PATH_MAX];
  usize path_length;
  const struct vfs_filesystem *filesystem;
  void *fs;
};

static const struct vfs_filesystem *filesystems[VFS_MAX_FILESYSTEMS];
static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static int filesystem_count;
static int mount_count;

static int copy_path(char *out, usize capacity, const char *path) {
  if (!out || !path || capacity < 2)
    return -1;
  usize length = strlen(path);
  if (length == 0 || length >= capacity)
    return -1;
  memcpy(out, path, length + 1);
  while (length > 1 && out[length - 1] == '/')
    out[--length] = 0;
  if (out[0] != '/')
    return -1;
  return 0;
}

void vfs_init(void) {
  log_write("vfs: init", KERNEL, LOG_INFO);
  memset(filesystems, 0, sizeof(filesystems));
  memset(mounts, 0, sizeof(mounts));
  filesystem_count = 0;
  mount_count = 0;
}

static const struct vfs_filesystem *find_filesystem(const char *name) {
  for (int i = 0; i < filesystem_count; i++)
    if (strcmp(filesystems[i]->name, name) == 0)
      return filesystems[i];
  return 0;
}

int vfs_register(const struct vfs_filesystem *filesystem) {
  if (!filesystem || !filesystem->name || !filesystem->operations ||
      !filesystem->mount || filesystem_count >= VFS_MAX_FILESYSTEMS ||
      find_filesystem(filesystem->name))
    return -1;
  filesystems[filesystem_count++] = filesystem;
  return 0;
}

static int attach_type(const char *mountpoint,
                       const struct vfs_filesystem *filesystem, void *fs) {
  if (!filesystem || !fs || mount_count >= VFS_MAX_MOUNTS)
    return -1;
  struct vfs_mount candidate;
  memset(&candidate, 0, sizeof(candidate));
  if (copy_path(candidate.path, sizeof(candidate.path), mountpoint) != 0)
    return -1;
  candidate.path_length = strlen(candidate.path);
  for (int i = 0; i < mount_count; i++)
    if (strcmp(mounts[i].path, candidate.path) == 0)
      return -1;
  candidate.filesystem = filesystem;
  candidate.fs = fs;
  mounts[mount_count++] = candidate;
  return 0;
}

int vfs_attach(const char *mountpoint, const char *filesystem, void *fs) {
  return attach_type(mountpoint, find_filesystem(filesystem), fs);
}

int vfs_mount_image(const char *mountpoint, const char *filesystem, void *image,
                    usize size) {
  const struct vfs_filesystem *type = find_filesystem(filesystem);
  void *fs = 0;
  if (!type || !image || !size ||
      (type->probe && type->probe(image, size) <= 0) ||
      type->mount(image, size, &fs) != 0)
    return -1;
  if (attach_type(mountpoint, type, fs) == 0)
    return 0;
  if (type->unmount)
    type->unmount(fs);
  return -1;
}

int vfs_mount_auto(const char *mountpoint, void *image, usize size,
                   const char **mounted_type) {
  for (int i = 0; i < filesystem_count; i++) {
    const struct vfs_filesystem *type = filesystems[i];
    if (type->probe && type->probe(image, size) <= 0)
      continue;
    void *fs = 0;
    if (type->mount(image, size, &fs) != 0)
      continue;
    if (attach_type(mountpoint, type, fs) == 0) {
      if (mounted_type)
        *mounted_type = type->name;
      return 0;
    }
    if (type->unmount)
      type->unmount(fs);
  }
  return -1;
}

static int mount_matches(const struct vfs_mount *mount, const char *path) {
  if (mount->path_length == 1)
    return path[0] == '/';
  return strncmp(path, mount->path, mount->path_length) == 0 &&
         (path[mount->path_length] == 0 || path[mount->path_length] == '/');
}

static const struct vfs_mount *resolve_mount(const char *path,
                                             const char **relative) {
  if (!path || path[0] != '/')
    return 0;
  const struct vfs_mount *best = 0;
  for (int i = 0; i < mount_count; i++) {
    if (mount_matches(&mounts[i], path) &&
        (!best || mounts[i].path_length > best->path_length))
      best = &mounts[i];
  }
  if (!best)
    return 0;
  const char *rest = path + best->path_length;
  if (best->path_length == 1)
    rest = path;
  if (!*rest)
    rest = "/";
  if (relative)
    *relative = rest;
  return best;
}

static const struct vfs_operations *file_ops(const struct vfs_file *file) {
  return file && file->mount ? file->mount->filesystem->operations : 0;
}

static int open_common(const char *path, struct vfs_file *file, int create) {
  if (!file)
    return -1;
  memset(file, 0, sizeof(*file));
  const char *relative;
  const struct vfs_mount *mount = resolve_mount(path, &relative);
  if (!mount)
    return -1;
  const struct vfs_operations *ops = mount->filesystem->operations;
  int (*operation)(void *, const char *, struct vfs_file *) =
      create ? ops->create : ops->open;
  if (!operation)
    return -1;
  file->mount = mount;
  if (operation(mount->fs, relative, file) == 0)
    return 0;
  memset(file, 0, sizeof(*file));
  return -1;
}

int vfs_open(const char *path, struct vfs_file *file) {
  return open_common(path, file, 0);
}

int vfs_create(const char *path, struct vfs_file *file) {
  return open_common(path, file, 1);
}

void vfs_close(struct vfs_file *file) {
  const struct vfs_operations *ops = file_ops(file);
  if (ops && ops->close)
    ops->close(file->mount->fs, file);
  if (file)
    memset(file, 0, sizeof(*file));
}

usize vfs_read(struct vfs_file *file, void *buffer, usize length) {
  const struct vfs_operations *ops = file_ops(file);
  return ops && ops->read ? ops->read(file->mount->fs, file, buffer, length)
                          : 0;
}

usize vfs_write(struct vfs_file *file, const void *buffer, usize length) {
  const struct vfs_operations *ops = file_ops(file);
  return ops && ops->write ? ops->write(file->mount->fs, file, buffer, length)
                           : 0;
}

int vfs_seek(struct vfs_file *file, u64 position) {
  const struct vfs_operations *ops = file_ops(file);
  return ops && ops->seek ? ops->seek(file->mount->fs, file, position) : -1;
}

int vfs_truncate(struct vfs_file *file) {
  const struct vfs_operations *ops = file_ops(file);
  return ops && ops->truncate ? ops->truncate(file->mount->fs, file) : -1;
}

int vfs_stat(const char *path, struct vfs_stat *out) {
  const char *relative;
  const struct vfs_mount *mount = resolve_mount(path, &relative);
  const struct vfs_operations *ops = mount ? mount->filesystem->operations : 0;
  return ops && ops->stat ? ops->stat(mount->fs, relative, out) : -1;
}

int vfs_file_stat(struct vfs_file *file, struct vfs_stat *out) {
  const struct vfs_operations *ops = file_ops(file);
  if (!ops || !out)
    return -1;
  if (ops->file_stat)
    return ops->file_stat(file->mount->fs, file, out);
  memset(out, 0, sizeof(*out));
  out->size = file->size;
  out->inode = file->inode;
  out->type = file->type;
  out->attributes = file->attributes;
  return 0;
}

static int path_operation(const char *path, int operation_kind) {
  const char *relative;
  const struct vfs_mount *mount = resolve_mount(path, &relative);
  if (!mount)
    return -1;
  const struct vfs_operations *ops = mount->filesystem->operations;
  if (operation_kind == 0)
    return ops->unlink ? ops->unlink(mount->fs, relative) : -1;
  if (operation_kind == 1)
    return ops->mkdir ? ops->mkdir(mount->fs, relative) : -1;
  return ops->rmdir ? ops->rmdir(mount->fs, relative) : -1;
}

int vfs_unlink(const char *path) { return path_operation(path, 0); }
int vfs_mkdir(const char *path) { return path_operation(path, 1); }
int vfs_rmdir(const char *path) { return path_operation(path, 2); }

long vfs_read_dir_one(const char *path, u32 *index, struct vfs_dirent *out) {
  const char *relative;
  const struct vfs_mount *mount = resolve_mount(path, &relative);
  const struct vfs_operations *ops = mount ? mount->filesystem->operations : 0;
  return ops && ops->read_dir ? ops->read_dir(mount->fs, relative, index, out)
                              : -1;
}

long vfs_read_dir(const char *path, u32 *index, char *buffer, usize length) {
  if (!index || !buffer || length == 0)
    return -1;
  usize written = 0;
  for (;;) {
    u32 before = *index;
    struct vfs_dirent entry;
    long status = vfs_read_dir_one(path, index, &entry);
    if (status <= 0)
      return status < 0 && written == 0 ? -1 : (long)written;
    usize name_length = strlen(entry.name);
    usize required =
        name_length + 1 + (entry.type == VFS_NODE_DIRECTORY ? 1 : 0);
    if (required > length - written) {
      *index = before;
      return written ? (long)written : -1;
    }
    memcpy(buffer + written, entry.name, name_length);
    written += name_length;
    if (entry.type == VFS_NODE_DIRECTORY)
      buffer[written++] = '/';
    buffer[written++] = 0;
  }
}
