/* rvpanel: Nano <-> Pico B panel link (UART, 1.5625 Mbaud 8N1).
 * Shared by the Nano image (src/) and the Pico B firmware (firmware/panel/).
 * Freestanding C11: stdint/stddef only.
 *
 * Packet = type u8, payload, CRC16 (CRC-16/CCITT-FALSE over type+payload,
 * little-endian). On the wire each packet is COBS-encoded and followed by a
 * 0x00 delimiter. Either side may send at any time; no acks. A receiver that
 * sees a bad CRC or COBS drops the packet and counts it.
 */
#ifndef RVPANEL_H
#define RVPANEL_H

#include <stddef.h>
#include <stdint.h>

#define RVPANEL_BAUD        1562500u    /* 25 MHz / 16 on the Nano: divisor 1 */

/* Nano -> Pico B */
#define RVPANEL_PAGE        0x01        /* display u8, page u8, data[128] */
#define RVPANEL_CONFIG      0x02        /* contrast u8 (0..255, all displays) */
/* Pico B -> Nano */
#define RVPANEL_ENC         0x81        /* id u8 (0..5), delta i8 */
#define RVPANEL_SW          0x82        /* id u8 (0..5), down u8 */
#define RVPANEL_MIDI        0x83        /* len u8 (1..3), bytes[3] (status first) */
#define RVPANEL_STATUS      0x84        /* rx_ok u32, crc_err u16, cobs_err u16, dropped u16 */

#define RVPANEL_PAGE_LEN    128u
#define RVPANEL_MAX_RAW     (1u + 2u + RVPANEL_PAGE_LEN + 2u)      /* 133: PAGE + CRC */
/* COBS: +1 code byte per 254 data bytes (+1), plus the 0x00 delimiter. */
#define RVPANEL_MAX_WIRE    (RVPANEL_MAX_RAW + RVPANEL_MAX_RAW / 254u + 2u)

static inline uint16_t rvpanel_crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFFu;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000u) ? (uint32_t)(c << 1) ^ 0x1021u : (uint32_t)c << 1);
    }
    return c;
}

/* Builds the wire bytes for one packet: COBS(type, payload, crc16) + 0x00.
 * out must hold RVPANEL_MAX_WIRE bytes; len <= RVPANEL_MAX_RAW - 3.
 * Returns the number of bytes written. */
static inline size_t rvpanel_encode(uint8_t type, const void *payload, size_t len, uint8_t *out)
{
    uint8_t raw[RVPANEL_MAX_RAW];
    const uint8_t *pl = (const uint8_t *)payload;
    size_t n = 0;
    raw[n++] = type;
    for (size_t i = 0; i < len; i++)
        raw[n++] = pl[i];
    uint16_t c = rvpanel_crc16(raw, n);
    raw[n++] = (uint8_t)c;
    raw[n++] = (uint8_t)(c >> 8);

    size_t o = 0, code_at = o++;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = raw[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    out[o++] = 0x00;
    return o;
}

/* Streaming decoder: feed bytes one at a time. */
typedef struct {
    uint8_t buf[RVPANEL_MAX_RAW];
    size_t n;            /* decoded bytes so far */
    uint8_t code, left;  /* current COBS block: code byte, data bytes still to come */
    uint8_t overflow;    /* this packet is too long: drop at the delimiter */
    uint32_t ok, crc_err, cobs_err;
} rvpanel_rx_t;

static inline void rvpanel_rx_init(rvpanel_rx_t *r)
{
    r->n = 0;
    r->code = 0;
    r->left = 0;
    r->overflow = 0;
    r->ok = r->crc_err = r->cobs_err = 0;
}

/* Returns 1 when a whole, CRC-good packet is in r->buf: type = buf[0],
 * payload = buf[1 .. *plen]. The packet stays valid until the next feed. */
static inline int rvpanel_rx_feed(rvpanel_rx_t *r, uint8_t b, size_t *plen)
{
    if (b == 0x00) {                        /* delimiter: end of packet */
        int good = 0;
        if (r->n == 0 && r->code == 0) {
            /* empty: idle delimiter, ignore */
        } else if (r->overflow || r->left != 0 || r->n < 3) {
            r->cobs_err++;
        } else {
            size_t n = r->n;
            uint16_t c = (uint16_t)(r->buf[n - 2] | r->buf[n - 1] << 8);
            if (rvpanel_crc16(r->buf, n - 2) == c) {
                r->ok++;
                *plen = n - 3;
                good = 1;
            } else {
                r->crc_err++;
            }
        }
        r->n = 0;
        r->code = 0;
        r->left = 0;
        r->overflow = 0;
        return good;
    }
    if (r->left == 0) {                     /* a code byte */
        if (r->code != 0 && r->code != 0xFF) {  /* previous block ended in an implied zero */
            if (r->n < sizeof r->buf) r->buf[r->n++] = 0;
            else r->overflow = 1;
        }
        r->code = b;
        r->left = (uint8_t)(b - 1);
        return 0;
    }
    if (r->n < sizeof r->buf) r->buf[r->n++] = b;
    else r->overflow = 1;
    r->left--;
    return 0;
}

#endif
