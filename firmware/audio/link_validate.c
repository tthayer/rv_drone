#include "link_validate.h"

#include <string.h>

link_result_t link_validate(link_stats_t *s, const rvlink_m2s_t *f) {
    if (!rvlink_check(f)) { s->crc_err++; return LINK_CRC; }
    if (f->magic != RVLINK_MAGIC_M2S) { s->magic_err++; return LINK_MAGIC; }
    if (f->flags & RVLINK_F_TEST) {
        const uint8_t *a = (const uint8_t *)f->audio;
        for (size_t i = 0; i < RVLINK_AUDIO_LEN; i++) {
            if (a[i] != rvlink_test_byte(f->seq, i)) { s->pattern_err++; return LINK_PATTERN; }
        }
    }
    if (s->have_seq && f->seq != (uint16_t)(s->last_seq + 1u)) s->seq_gaps++;
    s->last_seq = f->seq;
    s->have_seq = 1;
    s->frames_ok++;
    return LINK_OK;
}

static uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }

void link_build_reply(const link_stats_t *s, uint32_t underruns, rvlink_s2m_t *out) {
    memset(out, 0, sizeof *out);
    out->magic = RVLINK_MAGIC_S2M;
    out->seq_echo = s->last_seq;
    out->ring_fill = 0;                 // no ring until M5
    out->underruns = sat16(underruns);
    out->crc_errors = sat16(s->crc_err);
    for (size_t i = 0; i < sizeof out->pad; i++) out->pad[i] = (uint8_t)(0xA5u ^ i);
    rvlink_seal(out);
}
