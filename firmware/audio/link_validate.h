// rvlink frame validation and reply construction. Pure C (no Pico SDK), so the
// host test (make test-link) runs the exact code the Pico runs.
#ifndef LINK_VALIDATE_H
#define LINK_VALIDATE_H

#include <stdint.h>
#include "rvlink.h"

typedef struct {
    uint32_t frames_ok;
    uint32_t crc_err;
    uint32_t magic_err;
    uint32_t pattern_err;
    uint32_t short_err;     // aborted / short transactions (counted by the SPI layer)
    uint32_t seq_gaps;      // number of gap events (not missing-block count)
    uint16_t last_seq;      // seq of the last good frame
    uint8_t  have_seq;      // last_seq is valid
} link_stats_t;

typedef enum { LINK_OK = 0, LINK_CRC, LINK_MAGIC, LINK_PATTERN } link_result_t;

// Checks in order: CRC, magic, test pattern (if RVLINK_F_TEST). CRC goes first
// so a corrupted frame is always attributed to the CRC, never to a magic error
// caused by that same corruption. Only a fully good frame updates last_seq and
// the gap count.
link_result_t link_validate(link_stats_t *s, const rvlink_m2s_t *f);

// Fills and seals the next reply. pad[i] = (uint8_t)(0xA5 ^ i).
void link_build_reply(const link_stats_t *s, uint32_t underruns, uint32_t ring_fill,
                      rvlink_s2m_t *out);

#endif
