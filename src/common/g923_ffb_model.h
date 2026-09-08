/*
 * g923_ffb_model.h — synthesize classic force feedback from SCS telemetry (v2).
 *
 * Native macOS ETS2 has no FFB output of its own, but it DOES load telemetry
 * plugins. So we read the game's telemetry and drive the wheel ourselves. This
 * module is the pure, device-independent force model: telemetry in, classic
 * slot forces out (centering spring + engine rumble + road/bump jolts). It is
 * unit-testable without a wheel; the runner (g923_ffb_telemetry) feeds it and
 * sends the encoded commands via IOKit HID.
 *
 * This is SYNTHESIZED force from game state, not the game's own physics FFB.
 */
#ifndef G923_FFB_MODEL_H
#define G923_FFB_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "g923_effects.h"          /* g923_slot_forces, g923_encode_slots */
#include "g923_telemetry_shm.h"

typedef struct {
    float centering_gain;      /* 0..1 overall self-centering spring strength */
    float centering_speed_ref; /* km/h at which centering reaches ~full */
    float rumble_gain;         /* 0..1 engine-rpm rumble amplitude */
    float bump_gain;           /* 0..1 road/suspension jolt amplitude */
    bool  invert;              /* flip force sign if the wheel pulls the wrong way */
} g923_ffb_config;

typedef struct {
    g923_ffb_config cfg;
    double phase;              /* rumble oscillator phase (radians) */
} g923_ffb_model;

/* Fill cfg with sensible defaults (good starting point; tune on hardware). */
void g923_ffb_config_defaults(g923_ffb_config *cfg);

/* Init the model. If cfg is NULL, defaults are used. */
void g923_ffb_model_init(g923_ffb_model *m, const g923_ffb_config *cfg);

/* Advance the model by dt_s seconds using a telemetry snapshot and the derived
 * road roughness (0..1), producing the classic slot forces for this tick.
 * When the game is not connected or is paused, produces no force. */
void g923_ffb_model_step(g923_ffb_model *m, const g923_telemetry_shm *snap,
                         float roughness, double dt_s, g923_slot_forces *out);

#endif /* G923_FFB_MODEL_H */
