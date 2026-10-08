#include "preset_fs.h"
#include "ff.h"
#include "diskio.h"
#include "sd.h"
#include "uart.h"

static FATFS fs;
static int mounted;

int preset_fs_mount(void)
{
    if (mounted) return 0;
    FRESULT r = f_mount(&fs, "", 1);
    if (r != FR_OK) return (int)r;
    f_mkdir("/presets");                     /* FR_EXIST is fine */
    mounted = 1;
    return 0;
}

static void path(char *p, const char *name)
{
    const char *d = "/presets/";
    while (*d) *p++ = *d++;
    while (*name) *p++ = *name++;
    *p = 0;
}

static int st_read(const char *name, char *buf, int max)
{
    if (preset_fs_mount()) return -1;
    char p[24];
    path(p, name);
    FIL f;
    FRESULT r = f_open(&f, p, FA_READ);
    if (r == FR_NO_FILE) return -2;
    if (r != FR_OK) { mounted = 0; return -1; }
    UINT n = 0;
    r = f_read(&f, buf, (UINT)max, &n);
    f_close(&f);
    return r == FR_OK ? (int)n : -1;
}

static int st_write(const char *name, const char *buf, int len)
{
    if (preset_fs_mount()) return -1;
    char p[24];
    path(p, name);
    FIL f;
    if (f_open(&f, p, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { mounted = 0; return -1; }
    UINT n = 0;
    FRESULT r = f_write(&f, buf, (UINT)len, &n);
    FRESULT c = f_close(&f);                 /* flushes FAT + directory */
    return (r == FR_OK && c == FR_OK && n == (UINT)len) ? 0 : -1;
}

const ui_store_t preset_fs_store = { st_read, st_write };

int preset_fs_format(void)
{
    static BYTE work[32768];
    mounted = 0;
    f_mount(0, "", 0);
    if (disk_initialize(0) & STA_NOINIT) return -1;
    MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
    FRESULT r = f_mkfs("", &opt, work, sizeof work);
    if (r != FR_OK) return (int)r;             /* FRESULT 1..19 */
    int m = preset_fs_mount();
    return m ? 100 + m : 0;                    /* 100+: formatted, mount failed */
}

void preset_fs_info(void)
{
    int m = preset_fs_mount();
    const sd_info_t *i = sd_info();
    uart_puts("sd: ");
    if (!i->sectors) {
        uart_puts("no card (mount ");
        uart_put_dec((uint64_t)m);
        uart_puts(")\n");
        sd_dump();
        return;
    }
    uart_put_dec(i->sectors / 2048);
    uart_puts(" MiB, ");
    uart_puts(i->sdhc ? "SDHC/XC, " : "SDSC, ");
    uart_puts(i->bus4 ? "4-bit, " : "1-bit, ");
    uart_put_dec(i->clk_hz / 1000);
    uart_puts(" kHz (base ");
    uart_put_dec(i->base_hz / 1000000);
    uart_puts(" MHz); ");
    if (m) {
        uart_puts("mount failed (FRESULT ");
        uart_put_dec((uint64_t)m);
        uart_puts("; 'F' twice to format)\n");
        return;
    }
    DWORD fre;
    FATFS *pfs;
    if (f_getfree("", &fre, &pfs) == FR_OK) {
        uart_puts(pfs->fs_type == FS_FAT32 ? "FAT32, " : "FAT, ");
        uart_put_dec((uint64_t)fre * pfs->csize / 2048);
        uart_puts(" MiB free\n");
    } else {
        uart_puts("mounted\n");
    }
}
