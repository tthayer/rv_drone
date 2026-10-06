/* rvlink: Nano (SPI master) <-> Pico A (SPI slave) audio link.
 * Shared by the Nano image (src/) and the Pico firmware (firmware/).
 * Freestanding C11: stdint/stddef only.
 *
 * Wire format: SPI mode 3 (CPOL=1, CPHA=1), MSB first, 8-bit frames.
 * One transaction = RVLINK_FRAME_LEN bytes each way, CS held low throughout.
 * Pico A raises DRQ (rising edge) when it wants a block and its reply is
 * already loaded. Multi-byte fields are little-endian. The CRC is CRC-32/ISO-HDLC
 * (zlib crc32) over bytes [0, RVLINK_FRAME_LEN - 4), stored in the last 4 bytes.
 */
#ifndef RVLINK_H
#define RVLINK_H

#include <stddef.h>
#include <stdint.h>

#define RVLINK_FRAME_LEN     528u
#define RVLINK_BLOCK_FRAMES  64u            /* stereo frames per block */
#define RVLINK_AUDIO_LEN     (RVLINK_BLOCK_FRAMES * 2u * 4u)   /* 512 */

#define RVLINK_MAGIC_M2S     0x314C5652u    /* "RVL1" */
#define RVLINK_MAGIC_S2M     0x31415652u    /* "RVA1" */

/* flags (master -> slave) */
#define RVLINK_F_TEST        0x0001u        /* audio[] holds the test pattern */

/* Nano -> Pico A */
typedef struct {
    uint32_t magic;                         /* RVLINK_MAGIC_M2S */
    uint16_t seq;
    uint16_t flags;
    int32_t  audio[RVLINK_BLOCK_FRAMES * 2]; /* L,R interleaved, 24-bit left-aligned */
    uint32_t pad;
    uint32_t crc;
} rvlink_m2s_t;

/* Pico A -> Nano (prepared before DRQ, so it describes the previous block) */
typedef struct {
    uint32_t magic;                         /* RVLINK_MAGIC_S2M */
    uint16_t seq_echo;                      /* seq of the last good block received */
    uint8_t  ring_fill;                     /* blocks queued for I2S */
    uint8_t  pad0;
    uint16_t underruns;
    uint16_t crc_errors;
    uint8_t  pad[RVLINK_FRAME_LEN - 16];
    uint32_t crc;
} rvlink_s2m_t;

_Static_assert(sizeof(rvlink_m2s_t) == RVLINK_FRAME_LEN, "m2s size");
_Static_assert(sizeof(rvlink_s2m_t) == RVLINK_FRAME_LEN, "s2m size");

/* Test pattern for M4 bring-up: audio byte i of block seq. */
static inline uint8_t rvlink_test_byte(uint16_t seq, size_t i)
{
    return (uint8_t)(seq + i * 7u);
}

/* CRC-32/ISO-HDLC, bitwise (small; fast enough for 528 B at 750 Hz). */
static inline uint32_t rvlink_crc32(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static inline void rvlink_seal(void *frame)
{
    uint8_t *f = (uint8_t *)frame;
    uint32_t c = rvlink_crc32(f, RVLINK_FRAME_LEN - 4);
    f[RVLINK_FRAME_LEN - 4] = (uint8_t)c;
    f[RVLINK_FRAME_LEN - 3] = (uint8_t)(c >> 8);
    f[RVLINK_FRAME_LEN - 2] = (uint8_t)(c >> 16);
    f[RVLINK_FRAME_LEN - 1] = (uint8_t)(c >> 24);
}

static inline int rvlink_check(const void *frame)
{
    const uint8_t *f = (const uint8_t *)frame;
    uint32_t c = (uint32_t)f[RVLINK_FRAME_LEN - 4] | (uint32_t)f[RVLINK_FRAME_LEN - 3] << 8 |
                 (uint32_t)f[RVLINK_FRAME_LEN - 2] << 16 | (uint32_t)f[RVLINK_FRAME_LEN - 1] << 24;
    return rvlink_crc32(f, RVLINK_FRAME_LEN - 4) == c;
}

#endif
