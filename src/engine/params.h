/* Parameter model shared by the encoders, MIDI CCs and (M8) presets. Each
 * parameter has a normalised position 0..1 (what an encoder moves) and a value
 * in its own unit (what the engine uses). */
#ifndef PARAMS_H
#define PARAMS_H

enum {
    /* OSC */
    P_DETUNE, P_DRIFT, P_SHAPE, P_OSCS, P_SUB, P_SPREAD,
    /* FILTER */
    P_CUTOFF, P_RESO, P_FMOD_RATE, P_FMOD_DEPTH, P_DRIVE, P_FMODE,
    /* SPACE */
    P_CHORUS, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_REV_SIZE, P_REV_MIX,
    /* AMP */
    P_ATTACK, P_RELEASE, P_LATCH, P_TRANSPOSE, P_DAMP, P_VOLUME,
    /* CLOCK (MIDI clock follower) */
    P_SYNC, P_LFO_DIV, P_DLY_DIV,
    /* (same page) oscillator interval stacking */
    P_STACK,
    P_COUNT
};

typedef enum { CURVE_LIN, CURVE_EXP, CURVE_INT, CURVE_ENUM } param_curve_t;
typedef enum { UNIT_NONE, UNIT_CENTS, UNIT_HZ, UNIT_MS, UNIT_S, UNIT_PCT, UNIT_ST } param_unit_t;

typedef struct {
    const char *name;           /* <= 8 chars: fits a 64 px half at 6 px/char */
    float min, max, def;        /* in value units */
    param_curve_t curve;        /* EXP: min > 0, log-spaced */
    param_unit_t unit;
    const char *const *labels;  /* CURVE_ENUM: one per integer value */
    float step;                 /* normalised change per encoder detent */
} param_desc_t;

#define PAGE_COUNT 5     /* the last page may hold fewer than 6 (empty halves) */
#define PAGE_PARAMS 6
extern const char *const page_names[PAGE_COUNT];

const param_desc_t *param_desc(int id);
float param_to_value(int id, float norm);     /* 0..1 -> value */
float param_to_norm(int id, float value);
/* Formats value into buf (>= 12 bytes), e.g. "1.2k", "850ms", "72%". */
void  param_format(int id, float value, char *buf);
/* Clock divisions: index (the parameter value) -> beats, 0 = free running. */
float param_lfo_div_beats(int idx);
float param_dly_div_beats(int idx);

/* MIDI CC -> parameter id, or -1. */
int   param_for_cc(int cc);
#endif
