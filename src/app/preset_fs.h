#ifndef PRESET_FS_H
#define PRESET_FS_H
#include "ui.h"

/* ui_store_t on FatFs: files in /presets on the SD card. Mounts lazily (and
 * again after a failure), so a card inserted later is picked up. */
extern const ui_store_t preset_fs_store;
int  preset_fs_mount(void);     /* 0 = mounted, else FRESULT / sd_init step */
int  preset_fs_format(void);    /* f_mkfs (FAT32, one MBR partition) + /presets */
void preset_fs_info(void);      /* card + volume summary to the console */
#endif
