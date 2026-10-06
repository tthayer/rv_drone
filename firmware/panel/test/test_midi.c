// Host test for midi_parser.c: cc -I.. test_midi.c ../midi_parser.c
#include <stdio.h>
#include <string.h>

#include "midi_parser.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Feeds bytes, collecting emitted messages.
static int feed(midi_parser_t *p, const uint8_t *b, size_t n, midi_msg_t *out, int max) {
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        midi_msg_t m;
        if (midi_parser_feed(p, b[i], &m) && k < max) out[k++] = m;
    }
    return k;
}
#define FEED(p, arr, out) feed(p, arr, sizeof arr, out, (int)(sizeof out / sizeof out[0]))

int main(void) {
    midi_parser_t p;
    midi_msg_t m[16];
    int n;

    // Basic messages
    midi_parser_init(&p);
    { const uint8_t b[] = {0x90, 60, 100, 0x80, 60, 64, 0xB2, 74, 33, 0xC5, 12, 0xE1, 0x00, 0x40};
      n = FEED(&p, b, m); }
    CHECK(n == 5);
    CHECK(m[0].type == MIDI_NOTE_ON && m[0].ch == 0 && m[0].d1 == 60 && m[0].d2 == 100);
    CHECK(m[1].type == MIDI_NOTE_OFF && m[1].d1 == 60 && m[1].d2 == 64);
    CHECK(m[2].type == MIDI_CC && m[2].ch == 2 && m[2].d1 == 74 && m[2].d2 == 33);
    CHECK(m[3].type == MIDI_PROGRAM_CHANGE && m[3].ch == 5 && m[3].d1 == 12);
    CHECK(m[4].type == MIDI_PITCH_BEND && m[4].ch == 1 && m[4].value == 0);   // 0x2000 centre

    // Pitch bend extremes
    midi_parser_init(&p);
    { const uint8_t b[] = {0xE0, 0x00, 0x00, 0x7F, 0x7F};
      n = FEED(&p, b, m); }
    CHECK(n == 2 && m[0].value == -8192 && m[1].value == 8191);

    // Running status: one status byte, several messages; Note On vel 0 = Note Off
    midi_parser_init(&p);
    { const uint8_t b[] = {0x91, 60, 100, 62, 90, 60, 0, 64, 1};
      n = FEED(&p, b, m); }
    CHECK(n == 4);
    CHECK(m[0].type == MIDI_NOTE_ON && m[0].ch == 1 && m[0].d1 == 60);
    CHECK(m[1].type == MIDI_NOTE_ON && m[1].ch == 1 && m[1].d1 == 62 && m[1].d2 == 90);
    CHECK(m[2].type == MIDI_NOTE_OFF && m[2].d1 == 60 && m[2].d2 == 0);
    CHECK(m[3].type == MIDI_NOTE_ON && m[3].d1 == 64 && m[3].d2 == 1);

    // Running status with program change (1 data byte)
    midi_parser_init(&p);
    { const uint8_t b[] = {0xC0, 1, 2, 3};
      n = FEED(&p, b, m); }
    CHECK(n == 3 && m[2].d1 == 3);

    // Realtime bytes interleaved everywhere, including mid-message and
    // between running-status messages; counted, never emitted
    midi_parser_init(&p);
    { const uint8_t b[] = {0xF8, 0x90, 0xF8, 60, 0xF8, 0xFE, 100, 0xF8,
                           62, 0xF8, 90, 0xFA, 0xFC, 0xF8};
      n = FEED(&p, b, m); }
    CHECK(n == 2);
    CHECK(m[0].type == MIDI_NOTE_ON && m[0].d1 == 60 && m[0].d2 == 100);
    CHECK(m[1].type == MIDI_NOTE_ON && m[1].d1 == 62 && m[1].d2 == 90);
    CHECK(p.clock_count == 6);
    CHECK(p.rt_other == 3);         // FE FA FC

    // SysEx is skipped (with realtime inside); ends at F7; no running status after it
    midi_parser_init(&p);
    { const uint8_t b[] = {0x90, 60, 100, 0xF0, 0x7E, 0x00, 0xF8, 0x09, 0x01, 0xF7,
                           61, 100,            // stray data: running status was cancelled
                           0x90, 62, 80};
      n = FEED(&p, b, m); }
    CHECK(n == 2);
    CHECK(m[0].d1 == 60);
    CHECK(m[1].d1 == 62 && m[1].d2 == 80);
    CHECK(p.sysex_count == 1 && p.clock_count == 1);

    // SysEx aborted by a status byte (no F7): the new status is honoured
    midi_parser_init(&p);
    { const uint8_t b[] = {0xF0, 1, 2, 3, 0x92, 70, 71};
      n = FEED(&p, b, m); }
    CHECK(n == 1 && m[0].ch == 2 && m[0].d1 == 70 && m[0].d2 == 71);

    // A status byte interrupts a half-received message
    midi_parser_init(&p);
    { const uint8_t b[] = {0x90, 60, 0xB0, 7, 99};
      n = FEED(&p, b, m); }
    CHECK(n == 1 && m[0].type == MIDI_CC && m[0].d1 == 7 && m[0].d2 == 99);

    // Data with no status at all is ignored
    midi_parser_init(&p);
    { const uint8_t b[] = {1, 2, 3, 0x90, 5, 6};
      n = FEED(&p, b, m); }
    CHECK(n == 1 && m[0].d1 == 5);

    // System common (song position, 2 data bytes) is consumed, cancels running status
    midi_parser_init(&p);
    { const uint8_t b[] = {0x90, 60, 100, 0xF2, 1, 2, 61, 100, 0xF6, 62, 100};
      n = FEED(&p, b, m); }
    CHECK(n == 1 && m[0].d1 == 60);

    // Pressure messages are consumed silently and keep alignment
    midi_parser_init(&p);
    { const uint8_t b[] = {0xD0, 5, 0xA0, 60, 10, 0x90, 60, 100};
      n = FEED(&p, b, m); }
    CHECK(n == 1 && m[0].type == MIDI_NOTE_ON);

    printf("midi: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
