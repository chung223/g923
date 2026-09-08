/*
 * g923_effects.h — device-independent force-feedback effect model.
 *
 * Holds downloaded effects, steps them over time, and collapses them into the
 * four Logitech firmware slots (one combined output force + spring + damper +
 * friction). This is the pure-computation part of the ForceFeedback plugin, so
 * it is unit-tested without a wheel or the framework.
 *
 * Effect parameters use DirectInput / IOForceFeedback scale:
 *   magnitudes/levels: signed, -10000..10000 (FF_FFNOMINALMAX)
 *   durations/times:   microseconds (FF_SECONDS == 1000000), or G923_INFINITE
 *   gain:              0..10000
 *   phase:             0..35999 (hundredths of a degree)
 */
#ifndef G923_EFFECTS_H
#define G923_EFFECTS_H

#include <stdint.h>
#include <stdbool.h>
#include "g923_protocol.h"

#define G923_MAX_EFFECTS   64
#define G923_INFINITE      0xFFFFFFFFu
#define G923_NOMINAL_MAX   10000

typedef enum {
    G923_FX_NONE = 0,
    G923_FX_CONSTANT,
    G923_FX_RAMP,
    G923_FX_PERIODIC,
    G923_FX_SPRING,
    G923_FX_DAMPER,
    G923_FX_FRICTION,
    G923_FX_INERTIA,   /* folded onto damper on this hardware */
} g923_effect_kind;

typedef enum {
    G923_WAVE_SINE = 0,
    G923_WAVE_SQUARE,
    G923_WAVE_TRIANGLE,
    G923_WAVE_SAWUP,
    G923_WAVE_SAWDOWN,
} g923_wave;

typedef struct {
    bool present;
    int32_t attack_level;   /* 0..10000 */
    uint32_t attack_time;   /* us */
    int32_t fade_level;     /* 0..10000 */
    uint32_t fade_time;     /* us */
} g923_envelope;

typedef struct {
    g923_effect_kind kind;
    bool allocated;
    bool playing;
    uint32_t duration;      /* us or G923_INFINITE */
    uint32_t start_delay;   /* us */
    int32_t gain;           /* 0..10000, per-effect */
    int32_t direction_deg;  /* polar direction in degrees; sign of force */
    g923_envelope env;

    /* constant */
    int32_t constant_level; /* -10000..10000 */
    /* ramp */
    int32_t ramp_start, ramp_end;
    /* periodic */
    g923_wave wave;
    int32_t periodic_magnitude; /* 0..10000 */
    int32_t periodic_offset;    /* -10000..10000 */
    uint32_t periodic_period;   /* us */
    uint32_t periodic_phase;    /* 0..35999 */
    /* condition (spring/damper/friction/inertia) */
    int32_t cond_center;    /* -10000..10000 offset (position) */
    int32_t cond_pos_coeff, cond_neg_coeff; /* -10000..10000 */
    int32_t cond_pos_sat, cond_neg_sat;     /* 0..10000 */
    int32_t cond_deadband;  /* 0..10000 */

    /* runtime */
    uint64_t start_time_us;
    uint32_t iterations;
} g923_effect;

typedef struct {
    g923_effect effects[G923_MAX_EFFECTS];
    int32_t device_gain;    /* 0..10000 */
    bool actuators_on;
    bool paused;
} g923_engine;

/* The slot forces produced for one tick; any has_* may be false. */
typedef struct {
    bool     has_constant;
    int32_t  constant_level;   /* signed s16 firmware scale */
    bool     has_spring;
    int32_t  spring_k1, spring_k2, spring_d1, spring_d2, spring_clip;
    bool     has_damper;
    int32_t  damper_k1, damper_k2, damper_clip;
    bool     has_friction;
    int32_t  friction_k1, friction_k2, friction_clip;
} g923_slot_forces;

void g923_engine_init(g923_engine *e);

int  g923_engine_alloc(g923_engine *e);           /* handle >=1, or 0 if full */
void g923_engine_free(g923_engine *e, int handle);
g923_effect *g923_engine_get(g923_engine *e, int handle);

void g923_engine_start(g923_engine *e, int handle, uint32_t iterations, uint64_t now_us);
void g923_engine_stop(g923_engine *e, int handle);
void g923_engine_stop_all(g923_engine *e);
void g923_engine_reset(g923_engine *e);

/* Compute combined slot forces at time now_us. */
void g923_engine_tick(g923_engine *e, uint64_t now_us, g923_slot_forces *out);

/* Encode slot forces into up to 4 wheel commands (0=constant,1=spring,
 * 2=damper,3=friction). cmds must hold 4*G923_CMD_LEN bytes. Returns count. */
int  g923_encode_slots(const g923_slot_forces *f, uint8_t *cmds);

#endif /* G923_EFFECTS_H */
