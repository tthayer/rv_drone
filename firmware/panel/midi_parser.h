// MIDI byte-stream parser: running status, realtime bytes interleaved anywhere,
// SysEx skipped. Pure C, no hardware: it is unit-tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MIDI_NOTE_OFF = 0,      // d1 note, d2 velocity (Note On with velocity 0 too)
    MIDI_NOTE_ON,           // d1 note, d2 velocity (1..127)
    MIDI_CC,                // d1 controller, d2 value
    MIDI_PITCH_BEND,        // value -8192..8191
    MIDI_PROGRAM_CHANGE,    // d1 program
} midi_type_t;

typedef struct {
    midi_type_t type;
    uint8_t ch;             // 0..15
    uint8_t d1, d2;
    int16_t value;          // pitch bend only
} midi_msg_t;

typedef struct {
    uint8_t status;         // running status, 0 = none
    uint8_t data[2];
    uint8_t have, need;
    bool in_sysex;
    uint32_t clock_count;   // 0xF8
    uint32_t rt_other;      // other realtime bytes (0xF9..0xFF)
    uint32_t sysex_count;   // SysEx messages started
    uint32_t msg_count;     // channel messages emitted
} midi_parser_t;

void midi_parser_init(midi_parser_t *p);

// Feeds one byte. Returns true when *out holds a complete message of one of
// the types above (other channel messages are consumed silently).
bool midi_parser_feed(midi_parser_t *p, uint8_t b, midi_msg_t *out);
