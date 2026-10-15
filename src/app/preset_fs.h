#ifndef PRESET_FS_H
#define PRESET_FS_H
#include <stdint.h>
#include "ui.h"

/* ui_store_t on FatFs: files in /presets on the SD card. Mounts lazily (and
 * again after a failure), so a card inserted later is picked up. */
extern const ui_store_t preset_fs_store;
int  preset_fs_mount(void);     /* 0 = mounted, else FRESULT / sd_init step */
int  preset_fs_format(void);    /* f_mkfs (FAT32, one MBR partition) + /presets */
void preset_fs_info(void);      /* card + volume summary to the console */

/* Root-file streaming for the in-place firmware updater (firmware_update.c):
 * stage an image at the card root, verify it, then swap it in. One file open
 * at a time. All return 0 on success and -1 on I/O error; open also returns
 * -2 when the file does not exist. */
int preset_fs_root_open(const char *name, int for_write);
int preset_fs_root_write(const void *buf, unsigned len);
int preset_fs_root_read(void *buf, unsigned len, unsigned *got);
int preset_fs_root_close(void);
int preset_fs_root_size(const char *name, uint32_t *size);
int preset_fs_root_unlink(const char *name);
int preset_fs_root_rename(const char *from, const char *to);
#endif
