#include "midi_parser.h"

#include <string.h>

void midi_parser_init(midi_parser_t *p) {
    memset(p, 0, sizeof *p);
}

// Data bytes needed after a status byte; 0 for none.
static uint8_t data_len(uint8_t status) {
    if (status >= 0xF0) {
        switch (status) {
        case 0xF1: case 0xF3: return 1;     // MTC quarter frame, song select
        case 0xF2: return 2;                // song position
        default: return 0;
        }
    }
    uint8_t hi = status & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
}

bool midi_parser_feed(midi_parser_t *p, uint8_t b, midi_msg_t *out) {
    if (b >= 0xF8) {                        // realtime: never disturbs anything
        if (b == 0xF8) p->clock_count++;
        else p->rt_other++;
        return false;
    }

    if (b & 0x80) {                         // status byte
        p->in_sysex = false;                // any status ends a SysEx
        p->have = 0;
        if (b == 0xF0) {
            p->in_sysex = true;
            p->sysex_count++;
            p->status = 0;
        } else if (b == 0xF7) {
            p->status = 0;
        } else {
            p->need = data_len(b);
            // System common cancels running status; with no data bytes
            // (F4..F6) there is nothing left to wait for either.
            p->status = (b >= 0xF0 && p->need == 0) ? 0 : b;
        }
        return false;
    }

    // data byte
    if (p->in_sysex || p->status == 0) return false;
    p->data[p->have++] = b;
    if (p->have < p->need) return false;
    p->have = 0;

    uint8_t st = p->status;
    if (st >= 0xF0) {                       // system common: consumed, no running status
        p->status = 0;
        return false;
    }

    uint8_t ch = st & 0x0F;
    switch (st & 0xF0) {
    case 0x80: case 0x90:
        out->type = (st & 0xF0) == 0x90 && p->data[1] != 0 ? MIDI_NOTE_ON : MIDI_NOTE_OFF;
        out->d1 = p->data[0];
        out->d2 = p->data[1];
        out->value = 0;
        break;
    case 0xB0:
        out->type = MIDI_CC;
        out->d1 = p->data[0];
        out->d2 = p->data[1];
        out->value = 0;
        break;
    case 0xC0:
        out->type = MIDI_PROGRAM_CHANGE;
        out->d1 = p->data[0];
        out->d2 = 0;
        out->value = 0;
        break;
    case 0xE0:
        out->type = MIDI_PITCH_BEND;
        out->d1 = p->data[0];
        out->d2 = p->data[1];
        out->value = (int16_t)((p->data[0] | (p->data[1] << 7)) - 8192);
        break;
    default:                                // poly/channel pressure: not used
        return false;
    }
    out->ch = ch;
    p->msg_count++;
    return true;
}
