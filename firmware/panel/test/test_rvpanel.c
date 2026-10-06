// Host test for common/rvpanel.h: COBS + CRC16 round trips and error handling.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rvpanel.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Feeds wire bytes; returns the number of packets decoded, last one in *type/payload.
static int feed(rvpanel_rx_t *r, const uint8_t *w, size_t n, uint8_t *type, uint8_t *pl, size_t *pn) {
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len;
        if (rvpanel_rx_feed(r, w[i], &len)) {
            got++;
            *type = r->buf[0];
            memcpy(pl, r->buf + 1, len);
            *pn = len;
        }
    }
    return got;
}

int main(void) {
    // CRC-16/CCITT-FALSE check value.
    CHECK(rvpanel_crc16((const uint8_t *)"123456789", 9) == 0x29B1);

    rvpanel_rx_t r;
    rvpanel_rx_init(&r);
    uint8_t w[RVPANEL_MAX_WIRE], pl[RVPANEL_MAX_RAW], type;
    size_t pn;

    // Random payloads of every length, with plenty of zeros: exact round trip,
    // no 0x00 inside the encoding, size within bound.
    srand(1);
    for (size_t len = 0; len <= RVPANEL_MAX_RAW - 3; len++) {
        for (int rep = 0; rep < 20; rep++) {
            uint8_t in[RVPANEL_MAX_RAW];
            for (size_t i = 0; i < len; i++) in[i] = (rand() & 3) ? (uint8_t)rand() : 0;
            size_t n = rvpanel_encode(RVPANEL_PAGE, in, len, w);
            CHECK(n <= RVPANEL_MAX_WIRE && w[n - 1] == 0);
            int zero_inside = 0;
            for (size_t i = 0; i + 1 < n; i++) zero_inside |= w[i] == 0;
            CHECK(!zero_inside);
            CHECK(feed(&r, w, n, &type, pl, &pn) == 1);
            CHECK(type == RVPANEL_PAGE && pn == len && memcmp(pl, in, len) == 0);
        }
    }
    uint32_t ok0 = r.ok;
    CHECK(r.crc_err == 0 && r.cobs_err == 0);

    // A real PAGE: all-zero data (worst case for COBS overhead is no zeros).
    uint8_t page[2 + RVPANEL_PAGE_LEN] = { 2, 7 };
    size_t n = rvpanel_encode(RVPANEL_PAGE, page, sizeof page, w);
    CHECK(feed(&r, w, n, &type, pl, &pn) == 1 && pn == sizeof page && pl[0] == 2 && pl[1] == 7);

    // Corrupted byte: CRC error (or COBS error if it hit a code byte), no packet.
    uint8_t enc[2] = { 3, (uint8_t)-2 };
    n = rvpanel_encode(RVPANEL_ENC, enc, 2, w);
    w[2] ^= 0x40;
    CHECK(feed(&r, w, n, &type, pl, &pn) == 0 && r.crc_err + r.cobs_err == 1);

    // Joining mid-packet (garbage, then delimiter), then a good packet: resyncs.
    const uint8_t junk[] = { 0x05, 0x11, 0x22, 0x00 };
    CHECK(feed(&r, junk, sizeof junk, &type, pl, &pn) == 0);
    n = rvpanel_encode(RVPANEL_ENC, enc, 2, w);
    CHECK(feed(&r, w, n, &type, pl, &pn) == 1 && type == RVPANEL_ENC && pl[0] == 3 && (int8_t)pl[1] == -2);

    // Idle delimiters between packets are ignored, not errors.
    uint32_t errs = r.crc_err + r.cobs_err;
    const uint8_t idle[] = { 0, 0, 0 };
    CHECK(feed(&r, idle, 3, &type, pl, &pn) == 0 && r.crc_err + r.cobs_err == errs);

    // Oversized packet: dropped as a COBS error, the next one still decodes.
    uint8_t big[300];
    memset(big, 0x55, sizeof big);
    big[0] = 0xFF;                       // one 254-byte block, then more
    big[255] = 0x2D;
    big[sizeof big - 1] = 0;
    CHECK(feed(&r, big, sizeof big, &type, pl, &pn) == 0 && r.cobs_err == errs - r.crc_err + 1);
    CHECK(feed(&r, w, n, &type, pl, &pn) == 1);
    CHECK(r.ok > ok0);

    printf(fails ? "test_rvpanel: %d FAILED\n" : "test_rvpanel: all passed\n", fails);
    return fails != 0;
}
