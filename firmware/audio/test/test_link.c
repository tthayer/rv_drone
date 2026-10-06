// Host test for firmware/audio/link_validate.c (plain cc).
#include <stdio.h>
#include <string.h>

#include "link_validate.h"
#include "audio_ring.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Builds a frame exactly as the Nano will.
static void make_frame(rvlink_m2s_t *f, uint16_t seq, uint16_t flags) {
    memset(f, 0, sizeof *f);
    f->magic = RVLINK_MAGIC_M2S;
    f->seq = seq;
    f->flags = flags;
    uint8_t *a = (uint8_t *)f->audio;
    for (size_t i = 0; i < RVLINK_AUDIO_LEN; i++) a[i] = rvlink_test_byte(seq, i);
    rvlink_seal(f);
}

int main(void) {
    link_stats_t s;
    rvlink_m2s_t f;
    memset(&s, 0, sizeof s);

    // Good consecutive frames, including seq wrap 65535 -> 0.
    make_frame(&f, 65534, RVLINK_F_TEST); CHECK(link_validate(&s, &f) == LINK_OK);
    make_frame(&f, 65535, RVLINK_F_TEST); CHECK(link_validate(&s, &f) == LINK_OK);
    make_frame(&f, 0, RVLINK_F_TEST);     CHECK(link_validate(&s, &f) == LINK_OK);
    CHECK(s.frames_ok == 3 && s.seq_gaps == 0 && s.last_seq == 0);

    // Wrong seq (gap): frame is still good, gap counted.
    make_frame(&f, 5, RVLINK_F_TEST);     CHECK(link_validate(&s, &f) == LINK_OK);
    CHECK(s.seq_gaps == 1 && s.last_seq == 5 && s.frames_ok == 4);

    // Corrupted audio byte, CRC not resealed: CRC error, state unchanged.
    make_frame(&f, 6, RVLINK_F_TEST);
    ((uint8_t *)f.audio)[100] ^= 0x10;
    CHECK(link_validate(&s, &f) == LINK_CRC);
    CHECK(s.crc_err == 1 && s.last_seq == 5 && s.frames_ok == 4);

    // Corrupted CRC byte.
    make_frame(&f, 6, RVLINK_F_TEST);
    ((uint8_t *)&f)[RVLINK_FRAME_LEN - 1] ^= 0xFF;
    CHECK(link_validate(&s, &f) == LINK_CRC && s.crc_err == 2);

    // Wrong magic with valid CRC.
    make_frame(&f, 6, RVLINK_F_TEST);
    f.magic = RVLINK_MAGIC_S2M; rvlink_seal(&f);
    CHECK(link_validate(&s, &f) == LINK_MAGIC && s.magic_err == 1);

    // Pattern wrong but CRC valid (resealed).
    make_frame(&f, 6, RVLINK_F_TEST);
    ((uint8_t *)f.audio)[511] ^= 1; rvlink_seal(&f);
    CHECK(link_validate(&s, &f) == LINK_PATTERN && s.pattern_err == 1);

    // Without the TEST flag the audio is not pattern-checked.
    make_frame(&f, 6, 0);
    ((uint8_t *)f.audio)[3] ^= 0x55; rvlink_seal(&f);
    CHECK(link_validate(&s, &f) == LINK_OK && s.frames_ok == 5 && s.last_seq == 6);

    // Reply.
    rvlink_s2m_t r;
    s.short_err = 2;
    link_build_reply(&s, 70000, 4, &r);
    CHECK(r.magic == RVLINK_MAGIC_S2M && r.seq_echo == 6 && r.ring_fill == 4);
    CHECK(r.underruns == 0xFFFF && r.crc_errors == s.crc_err);
    int pad_ok = 1;
    for (size_t i = 0; i < sizeof r.pad; i++) pad_ok &= r.pad[i] == (uint8_t)(0xA5 ^ i);
    CHECK(pad_ok);
    CHECK(rvlink_check(&r));
    link_build_reply(&s, 3, 300, &r);
    CHECK(r.underruns == 3 && r.ring_fill == 0xFF);

    // Ring: priming, FIFO order, underrun -> re-prime, overflow.
    static audio_ring_t ring;
    int32_t in[AUDIO_RING_WORDS], out[AUDIO_RING_WORDS];
    for (int b = 0; b < (int)AUDIO_RING_TARGET - 1; b++) {
        for (unsigned i = 0; i < AUDIO_RING_WORDS; i++) in[i] = b * 1000 + (int)i;
        CHECK(audio_ring_push(&ring, in));
        audio_ring_pop(&ring, out);                     // still priming: silence
        CHECK(out[0] == 0 && out[5] == 0 && !ring.playing);
    }
    for (unsigned i = 0; i < AUDIO_RING_WORDS; i++) in[i] = 3000 + (int)i;
    CHECK(audio_ring_push(&ring, in));                  // reaches TARGET
    for (int b = 0; b < (int)AUDIO_RING_TARGET; b++) {
        audio_ring_pop(&ring, out);
        CHECK(ring.playing && out[0] == b * 1000 && out[127] == b * 1000 + 127);
    }
    audio_ring_pop(&ring, out);                         // empty while playing
    CHECK(ring.underruns == 1 && !ring.playing && out[0] == 0 && audio_ring_fill(&ring) == 0);
    for (unsigned b = 0; b < AUDIO_RING_BLOCKS; b++) CHECK(audio_ring_push(&ring, in));
    CHECK(!audio_ring_push(&ring, in) && ring.overflows == 1);
    audio_ring_pop(&ring, out);                         // re-primed, plays again
    CHECK(ring.playing && out[0] == 3000 && audio_ring_fill(&ring) == AUDIO_RING_BLOCKS - 1);

    printf(fails ? "test_link: %d FAILED\n" : "test_link: all passed\n", fails);
    return fails != 0;
}
