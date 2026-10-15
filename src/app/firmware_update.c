/* In-place Nano firmware update over the UART0 console ('u').
 *
 * The host streams an image after a 12-byte header:
 *   "RVFW", u32 size (LE), u32 crc32 (LE), then `size` bytes.
 * crc32 is CRC-32/ISO-HDLC (zlib), the same function rvlink_crc32() uses.
 *
 * Speed: the console runs at ~111.6 kbaud, so we switch UART0 to the fastest
 * integer divisor (div 1 = 1.5625 Mbaud on the 25 MHz clock, 13.5x) for the
 * transfer and restore it on any failure. The ready line carries the baud so
 * the host follows. Audio is interrupted on purpose (see fw_transfer): the
 * receive loop does not feed the audio link and the SD idle hook is cleared,
 * so the engine stops rendering and the link drains - the transfer is faster
 * and the CPU is free.
 *
 * We stage the image as /fip.new, verify it (received CRC, then re-read what
 * landed on the card), replace /fip.bin, and reboot. The SD card never leaves
 * the board. On any error /fip.bin is left untouched and UART0 is restored. */
#include "firmware_update.h"
#include "board.h"
#include "uart.h"
#include "timer.h"
#include "reset.h"
#include "audio_link.h"
#include "sd.h"
#include "preset_fs.h"

#define FW_MAGIC0 'R'
#define FW_MAGIC1 'V'
#define FW_MAGIC2 'F'
#define FW_MAGIC3 'W'

#define FW_STAGE   "/fip.new"
#define FW_TARGET  "/fip.bin"

#define FW_MAX_BYTES        (32u * 1024u * 1024u)     /* sanity cap */
#define FW_HEADER_TIMEOUT_US (15u * 1000000u)         /* magic + size + crc */
#define FW_BYTE_TIMEOUT_US   (5u * 1000000u)          /* per byte, body */
#define FW_CHUNK             512u

/* UART0 input clock (clk_cam0_200 = 25 MHz here; see uart_report_clock) and
 * the divisor used for the transfer. div 1 is the exact 1.5625 Mbaud rate,
 * which many USB-serial adapters (e.g. an FT232R) cannot hold cleanly in both
 * directions; div 2 (781250 baud) is the safe default. Lower it further (div 4
 * = 390625, ...) if needed. */
#define FW_UART_CLK_HZ       25000000u
#ifndef FW_UPLOAD_DIV
#define FW_UPLOAD_DIV        2u
#endif

static uint8_t chunk[FW_CHUNK];
static const char *fw_err;              /* fw_fail reason (reported at console baud) */
static uint32_t fw_ok_size, fw_ok_crc;

/* CRC-32/ISO-HDLC, incremental. */
static uint32_t crc_state;

static void crc_reset(void) { crc_state = 0xFFFFFFFFu; }

static void crc_update(const uint8_t *p, unsigned n)
{
    uint32_t c = crc_state;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    crc_state = c;
}

static uint32_t crc_final(void) { return ~crc_state; }

/* Read one UART byte. -1 on timeout. No audio: the link is deliberately
 * interrupted for the duration of the transfer. */
static int getc_timeout(uint32_t timeout_us)
{
    uint64_t deadline = rdtime() + (uint64_t)timeout_us * (BOARD_TIMEBASE_HZ / 1000000u);
    for (;;) {
        int c = uart_getc_nonblock();
        if (c >= 0) return c;
        if (rdtime() >= deadline) return -1;
    }
}

static int fw_fail(const char *msg)
{
    fw_err = msg;
    preset_fs_root_close();
    preset_fs_root_unlink(FW_STAGE);
    return -1;
}

/* Header, body, verify, swap. On success this reboots and does not return. */
static int fw_transfer(void)
{
    /* Header: magic, then size + crc. */
    static const char magic[4] = { FW_MAGIC0, FW_MAGIC1, FW_MAGIC2, FW_MAGIC3 };
    for (int i = 0; i < 4; i++) {
        int c = getc_timeout(FW_HEADER_TIMEOUT_US);
        if (c < 0) return fw_fail("no image");
        if ((char)c != magic[i]) return fw_fail("bad magic");
    }
    uint8_t hdr[8];
    for (int i = 0; i < 8; i++) {
        int c = getc_timeout(FW_HEADER_TIMEOUT_US);
        if (c < 0) return fw_fail("short header");
        hdr[i] = (uint8_t)c;
    }
    uint32_t size = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 |
                    (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    uint32_t want_crc = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 |
                        (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    if (size == 0 || size > FW_MAX_BYTES) return fw_fail("bad size");

    /* Stage to /fip.new, accumulating the CRC as it arrives. */
    if (preset_fs_root_open(FW_STAGE, 1) != 0) return fw_fail("open");
    crc_reset();
    uint32_t got = 0;
    uint32_t next_progress = 32768;
    while (got < size) {
        unsigned want = size - got;
        if (want > FW_CHUNK) want = FW_CHUNK;
        for (unsigned n = 0; n < want; n++) {
            int c = getc_timeout(FW_BYTE_TIMEOUT_US);
            if (c < 0) return fw_fail("timeout");
            chunk[n] = (uint8_t)c;
        }
        crc_update(chunk, want);
        if (preset_fs_root_write(chunk, want) != 0) return fw_fail("write");
        got += want;
        if (got >= next_progress) {
            uart_putc('.');
            next_progress += 32768;
        }
    }
    uart_putc('\n');
    if (preset_fs_root_close() != 0) return fw_fail("flush");
    if (crc_final() != want_crc) return fw_fail("crc");

    /* Re-read the staged file and check what is actually on the card. */
    uint32_t fsz = 0;
    if (preset_fs_root_size(FW_STAGE, &fsz) != 0 || fsz != size)
        return fw_fail("size");
    if (preset_fs_root_open(FW_STAGE, 0) != 0) return fw_fail("reopen");
    crc_reset();
    for (;;) {
        unsigned n = 0;
        if (preset_fs_root_read(chunk, FW_CHUNK, &n) != 0) {
            preset_fs_root_close();
            return fw_fail("read");
        }
        if (n == 0) break;
        crc_update(chunk, n);
    }
    preset_fs_root_close();
    if (crc_final() != want_crc) return fw_fail("verify");

    /* Swap in the verified image. The window between unlink and rename is
     * microseconds; a power loss there is recoverable by USB boot. */
    if (preset_fs_root_unlink(FW_TARGET) != 0) return fw_fail("unlink");
    if (preset_fs_root_rename(FW_STAGE, FW_TARGET) != 0) return fw_fail("rename");
    if (preset_fs_root_size(FW_TARGET, &fsz) != 0 || fsz != size)
        return fw_fail("final");

    fw_ok_size = size;
    fw_ok_crc = want_crc;
    return 0;                               /* the caller reports + reboots */
}

int firmware_update_receive(void)
{
    uint32_t orig_div = uart_divisor();
    uint32_t fast_baud = FW_UART_CLK_HZ / (16u * FW_UPLOAD_DIV);

    /* Announce the upload baud at the console rate, drain, then switch. */
    uart_puts("fwup: ready baud ");
    uart_put_dec(fast_baud);
    uart_puts("\n");
    uart_sync();
    uart_set_divisor(FW_UPLOAD_DIV);

    /* Interrupt the audio for the transfer: no audio_link_poll() in the
     * receive loop, and no render during SD waits (the hook is restored
     * below). The engine stops; Pico A just underruns until the reboot. */
    sd_set_idle_hook(0);

    int rc = fw_transfer();

    /* Report at the console baud: the fast upload baud's return path is not
     * reliable on every adapter (an FT232R can misread it), so the host
     * switches back to the console baud to read the result. */
    uart_set_divisor(orig_div);
    uart_async_tx(1);
    sd_set_idle_hook(audio_link_poll);
    if (rc == 0) {
        uart_puts("fwup: done ");
        uart_put_dec(fw_ok_size);
        uart_puts(" bytes, crc ");
        uart_put_hex(fw_ok_crc);
        uart_puts(", rebooting\n");
        uart_sync();                        /* flush before the reset */
        board_reset();
    }
    uart_puts("fwup: fail ");
    uart_puts(fw_err ? fw_err : "?");
    uart_putc('\n');
    return rc;
}
