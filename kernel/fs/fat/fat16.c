/* kernel/fs/fat/fat16.c , FAT16 backend selection.
 *
 * Binds the shared VFAT engine to FAT16's fixed root directory and 16-bit
 * allocation-table entries.
 */
#include "fat_internal.h"
static int fat16_init(u8 *image, usize size) {
  return fat_mount_format(image, size, FAT_TYPE_16);
}

const struct fat_ops fat16_ops = {
  .init = fat16_init,
  .alloc_cluster = fat_impl_alloc_cluster,
  .free_chain = fat_impl_free_chain,
  .find_entry = fat_impl_find_entry,
  .create_entry = fat_impl_create_entry,
  .erase_entry = fat_impl_erase_entry,
  .init_dir_cluster = fat_impl_init_dir_cluster,
  .read_dir = fat_impl_read_dir,
  .dir_is_empty = fat_impl_dir_is_empty,
  .read_dir_one = fat_impl_read_dir_one,
  .entry_get_cluster = fat_impl_entry_get_cluster,
  .entry_set_cluster = fat_impl_entry_set_cluster,
  .entry_get_size = fat_impl_entry_get_size,
  .entry_set_size = fat_impl_entry_set_size,
  .set_timestamp = fat_impl_set_timestamp,
};
