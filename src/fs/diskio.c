/* FatFs disk I/O glue: drive 0 = SD0 (src/hal/sd.c). */
#include "ff.h"
#include "diskio.h"
#include "sd.h"

static DSTATUS stat = STA_NOINIT;

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : stat; }

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv) return STA_NOINIT;
    stat = sd_init() == 0 ? 0 : STA_NOINIT;
    return stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (stat & STA_NOINIT)) return RES_NOTRDY;
    return sd_read((uint32_t)sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (stat & STA_NOINIT)) return RES_NOTRDY;
    return sd_write((uint32_t)sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || (stat & STA_NOINIT)) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;                   /* writes complete before return */
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)sd_info()->sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 8192; return RES_OK;   /* 4 MiB AU: mkfs alignment */
    }
    return RES_PARERR;
}
