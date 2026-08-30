#include <fs/ext2/ext2_internal.h>
#include <utilities/string.h>

static u16 directory_record_size(u8 name_length) {
    return (u16)((8u + name_length + 3u) & ~3u);
}

static int entry_valid(struct ext2_fs *fs, const struct ext2_dir_entry *entry,
                       u32 block_offset) {
    return entry && entry->record_length >= 8 &&
           (entry->record_length & 3u) == 0 &&
           entry->record_length <= fs->block_size - block_offset &&
           entry->name_length <= entry->record_length - 8;
}

static int entry_name_is(const struct ext2_dir_entry *entry,
                         const char *name) {
    usize length = strlen(name);
    return length == entry->name_length &&
           memcmp(entry->name, name, length) == 0;
}

static u8 inode_file_type(const struct ext2_inode *inode) {
    if (!inode)
        return EXT2_FT_UNKNOWN;
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return EXT2_FT_DIR;
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        return EXT2_FT_REG_FILE;
    return EXT2_FT_UNKNOWN;
}

static u8 entry_file_type(struct ext2_fs *fs,
                               const struct ext2_dir_entry *entry) {
    if (entry->file_type)
        return entry->file_type;
    return inode_file_type(ext2_inode_get(fs, entry->inode));
}

int ext2_dir_lookup(struct ext2_fs *fs, u32 directory_inode,
                    const char *name, u32 *inode_number_out,
                    u8 *type_out) {
    struct ext2_inode *directory = ext2_inode_get(fs, directory_inode);
    if (!directory || (directory->mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
        !name || !*name)
        return -1;
    u32 offset = 0;
    while (offset < directory->size) {
        u32 logical = offset / fs->block_size;
        u32 in_block = offset % fs->block_size;
        u32 number = ext2_inode_block(fs, directory, logical, 0);
        u8 *block = ext2_block(fs, number);
        struct ext2_dir_entry *entry =
            block ? (struct ext2_dir_entry *)(block + in_block) : 0;
        if (!entry_valid(fs, entry, in_block))
            return -1;
        if (entry->inode && entry_name_is(entry, name)) {
            if (inode_number_out)
                *inode_number_out = entry->inode;
            if (type_out)
                *type_out = entry_file_type(fs, entry);
            return 0;
        }
        offset += entry->record_length;
    }
    return -1;
}

static int next_component(const char **cursor, char out[VFS_NAME_MAX + 1]) {
    const char *p = *cursor;
    while (*p == '/')
        p++;
    if (!*p) {
        *cursor = p;
        return 0;
    }
    usize length = 0;
    while (*p && *p != '/') {
        if (length >= VFS_NAME_MAX)
            return -1;
        out[length++] = *p++;
    }
    out[length] = 0;
    *cursor = p;
    return 1;
}

int ext2_path_resolve(struct ext2_fs *fs, const char *path,
                      u32 *inode_number_out) {
    if (!fs || !path || !inode_number_out)
        return -1;
    u32 current = EXT2_ROOT_INODE;
    const char *cursor = path;
    char component[VFS_NAME_MAX + 1];
    int status;
    while ((status = next_component(&cursor, component)) > 0) {
        u8 type;
        if (ext2_dir_lookup(fs, current, component, &current, &type) != 0)
            return -1;
        while (*cursor == '/')
            cursor++;
        if (*cursor && type != EXT2_FT_DIR)
            return -1;
    }
    if (status < 0)
        return -1;
    *inode_number_out = current;
    return 0;
}

int ext2_path_parent(struct ext2_fs *fs, const char *path,
                     u32 *parent_inode_out,
                     char leaf[VFS_NAME_MAX + 1]) {
    if (!fs || !path || !parent_inode_out || !leaf)
        return -1;
    u32 current = EXT2_ROOT_INODE;
    const char *cursor = path;
    char component[VFS_NAME_MAX + 1];
    int status = next_component(&cursor, component);
    if (status <= 0)
        return -1;
    for (;;) {
        const char *lookahead = cursor;
        char next[VFS_NAME_MAX + 1];
        int next_status = next_component(&lookahead, next);
        if (next_status < 0)
            return -1;
        if (next_status == 0) {
            memcpy(leaf, component, strlen(component) + 1);
            *parent_inode_out = current;
            return strcmp(leaf, ".") && strcmp(leaf, "..") ? 0 : -1;
        }
        u8 type;
        if (ext2_dir_lookup(fs, current, component, &current, &type) != 0 ||
            type != EXT2_FT_DIR)
            return -1;
        memcpy(component, next, strlen(next) + 1);
        cursor = lookahead;
    }
}

static void initialize_entry(struct ext2_dir_entry *entry, u16 record,
                             u32 inode_number, u8 type,
                             const char *name, u8 name_length) {
    memset(entry, 0, record);
    entry->inode = inode_number;
    entry->record_length = record;
    entry->name_length = name_length;
    entry->file_type = type;
    memcpy(entry->name, name, name_length);
}

int ext2_dir_add(struct ext2_fs *fs, u32 directory_inode,
                 const char *name, u32 inode_number, u8 type) {
    struct ext2_inode *directory = ext2_inode_get(fs, directory_inode);
    usize name_length = name ? strlen(name) : 0;
    if (!directory || (directory->mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
        name_length == 0 || name_length > VFS_NAME_MAX ||
        ext2_dir_lookup(fs, directory_inode, name, 0, 0) == 0)
        return -1;
    u16 needed = directory_record_size((u8)name_length);
    u32 offset = 0;
    while (offset < directory->size) {
        u32 logical = offset / fs->block_size;
        u32 in_block = offset % fs->block_size;
        u32 number = ext2_inode_block(fs, directory, logical, 0);
        u8 *block = ext2_block(fs, number);
        struct ext2_dir_entry *entry =
            block ? (struct ext2_dir_entry *)(block + in_block) : 0;
        if (!entry_valid(fs, entry, in_block))
            return -1;
        if (!entry->inode && entry->record_length >= needed) {
            u16 available = entry->record_length;
            initialize_entry(entry, available, inode_number, type, name,
                             (u8)name_length);
            return 0;
        }
        u16 used = directory_record_size(entry->name_length);
        if (entry->inode && entry->record_length >= used + needed) {
            u16 available = entry->record_length - used;
            entry->record_length = used;
            struct ext2_dir_entry *created =
                (struct ext2_dir_entry *)((u8 *)entry + used);
            initialize_entry(created, available, inode_number, type, name,
                             (u8)name_length);
            return 0;
        }
        offset += entry->record_length;
    }

    u32 logical =
        (directory->size + fs->block_size - 1) / fs->block_size;
    u32 number = ext2_inode_block(fs, directory, logical, 1);
    u8 *block = ext2_block(fs, number);
    if (!block)
        return -1;
    initialize_entry((struct ext2_dir_entry *)block, (u16)fs->block_size,
                     inode_number, type, name, (u8)name_length);
    directory->size = (logical + 1) * fs->block_size;
    return 0;
}

int ext2_dir_remove(struct ext2_fs *fs, u32 directory_inode,
                    const char *name, u32 *inode_number_out,
                    u8 *type_out) {
    struct ext2_inode *directory = ext2_inode_get(fs, directory_inode);
    if (!directory || (directory->mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;
    u32 offset = 0;
    struct ext2_dir_entry *previous = 0;
    u32 previous_logical = UINT32_MAX;
    while (offset < directory->size) {
        u32 logical = offset / fs->block_size;
        u32 in_block = offset % fs->block_size;
        if (logical != previous_logical) {
            previous = 0;
            previous_logical = logical;
        }
        u8 *block = ext2_block(
            fs, ext2_inode_block(fs, directory, logical, 0));
        struct ext2_dir_entry *entry =
            block ? (struct ext2_dir_entry *)(block + in_block) : 0;
        if (!entry_valid(fs, entry, in_block))
            return -1;
        if (entry->inode && entry_name_is(entry, name)) {
            if (inode_number_out)
                *inode_number_out = entry->inode;
            if (type_out)
                *type_out = entry_file_type(fs, entry);
            if (previous)
                previous->record_length += entry->record_length;
            else
                entry->inode = 0;
            return 0;
        }
        previous = entry;
        offset += entry->record_length;
    }
    return -1;
}

int ext2_dir_empty(struct ext2_fs *fs, u32 directory_inode) {
    struct ext2_inode *directory = ext2_inode_get(fs, directory_inode);
    if (!directory || (directory->mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return 0;
    u32 offset = 0;
    while (offset < directory->size) {
        u32 logical = offset / fs->block_size;
        u32 in_block = offset % fs->block_size;
        u8 *block = ext2_block(
            fs, ext2_inode_block(fs, directory, logical, 0));
        struct ext2_dir_entry *entry =
            block ? (struct ext2_dir_entry *)(block + in_block) : 0;
        if (!entry_valid(fs, entry, in_block))
            return 0;
        if (entry->inode && !entry_name_is(entry, ".") &&
            !entry_name_is(entry, ".."))
            return 0;
        offset += entry->record_length;
    }
    return 1;
}

long ext2_dir_read_one(struct ext2_fs *fs, u32 directory_inode,
                       u32 *index, struct vfs_dirent *out) {
    struct ext2_inode *directory = ext2_inode_get(fs, directory_inode);
    if (!directory || !index || !out ||
        (directory->mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;
    u32 offset = *index;
    while (offset < directory->size) {
        u32 logical = offset / fs->block_size;
        u32 in_block = offset % fs->block_size;
        u8 *block = ext2_block(
            fs, ext2_inode_block(fs, directory, logical, 0));
        struct ext2_dir_entry *entry =
            block ? (struct ext2_dir_entry *)(block + in_block) : 0;
        if (!entry_valid(fs, entry, in_block))
            return -1;
        offset += entry->record_length;
        *index = offset;
        if (!entry->inode || entry_name_is(entry, ".") ||
            entry_name_is(entry, ".."))
            continue;
        memcpy(out->name, entry->name, entry->name_length);
        out->name[entry->name_length] = 0;
        out->inode = entry->inode;
        out->type = entry_file_type(fs, entry) == EXT2_FT_DIR
                        ? VFS_NODE_DIRECTORY
                        : VFS_NODE_FILE;
        return (long)entry->name_length + 1;
    }
    return 0;
}
