/* g923_effects.c — see g923_effects.h.
 * Output-type effects (constant, ramp, periodic) are summed into one combined
 * constant-force command each tick, exactly as new-lg4ff's timer collapses them.
 * Condition effects (spring/damper/friction/inertia) map to their own slots. */
#include "g923_effects.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void g923_engine_init(g923_engine *e) {
    memset(e, 0, sizeof(*e));
    e->device_gain = G923_NOMINAL_MAX;
    e->actuators_on = true;
    e->paused = false;
}

int g923_engine_alloc(g923_engine *e) {
    for (int i = 1; i < G923_MAX_EFFECTS; i++) {
        if (!e->effects[i].allocated) {
            memset(&e->effects[i], 0, sizeof(g923_effect));
            e->effects[i].allocated = true;
            e->effects[i].gain = G923_NOMINAL_MAX;
            e->effects[i].iterations = 1;
            return i;
        }
    }
    return 0;
}

void g923_engine_free(g923_engine *e, int h) {
    if (h > 0 && h < G923_MAX_EFFECTS) memset(&e->effects[h], 0, sizeof(g923_effect));
}

g923_effect *g923_engine_get(g923_engine *e, int h) {
    if (h > 0 && h < G923_MAX_EFFECTS && e->effects[h].allocated) return &e->effects[h];
    return NULL;
}

void g923_engine_start(g923_engine *e, int h, uint32_t iterations, uint64_t now_us) {
    g923_effect *fx = g923_engine_get(e, h);
    if (!fx) return;
    fx->playing = true;
    fx->start_time_us = now_us;
    fx->iterations = iterations ? iterations : 1;
}

void g923_engine_stop(g923_engine *e, int h) {
    g923_effect *fx = g923_engine_get(e, h);
    if (fx) fx->playing = false;
}

void g923_engine_stop_all(g923_engine *e) {
    for (int i = 1; i < G923_MAX_EFFECTS; i++)
        if (e->effects[i].allocated) e->effects[i].playing = false;
}

void g923_engine_reset(g923_engine *e) {
    int32_t gain = e->device_gain;
    memset(e->effects, 0, sizeof(e->effects));
    e->paused = false;
    e->actuators_on = true;
    e->device_gain = gain;
}

static int32_t envelope_scale(const g923_effect *fx, uint32_t elapsed_us) {
    if (!fx->env.present) return G923_NOMINAL_MAX;
    if (fx->env.attack_time > 0 && elapsed_us < fx->env.attack_time) {
        double t = (double)elapsed_us / (double)fx->env.attack_time;
        return (int32_t)(fx->env.attack_level + t * (G923_NOMINAL_MAX - fx->env.attack_level));
    }
    uint32_t dur = fx->duration;
    if (dur != G923_INFINITE && fx->env.fade_time > 0 && dur > fx->env.fade_time
        && elapsed_us > (dur - fx->env.fade_time)) {
        uint32_t into = elapsed_us - (dur - fx->env.fade_time);
        double t = (double)into / (double)fx->env.fade_time;
        return (int32_t)(G923_NOMINAL_MAX + t * (fx->env.fade_level - G923_NOMINAL_MAX));
    }
    return G923_NOMINAL_MAX;
}

static int32_t apply_gains(const g923_engine *e, const g923_effect *fx,
                           int32_t level, uint32_t elapsed_us) {
    int64_t v = level;
    v = v * envelope_scale(fx, elapsed_us) / G923_NOMINAL_MAX;
    v = v * fx->gain / G923_NOMINAL_MAX;
    v = v * e->device_gain / G923_NOMINAL_MAX;
    return (int32_t)v;
}

static int32_t direction_sign(const g923_effect *fx) {
    int d = ((fx->direction_deg % 360) + 360) % 360;
    return (d > 180) ? -1 : 1;
}

/* Is the effect active now? Fills *elapsed_us with the phase within the current
 * iteration/period. */
static bool effect_active(const g923_effect *fx, uint64_t now_us, uint32_t *elapsed_out) {
    if (!fx->allocated || !fx->playing) return false;
    if (now_us < fx->start_time_us) return false;
    uint64_t since = now_us - fx->start_time_us;
    if (since < fx->start_delay) return false;
    uint64_t run = since - fx->start_delay;
    uint32_t elapsed;
    if (fx->duration != G923_INFINITE && fx->duration > 0) {
        uint64_t total = (uint64_t)fx->duration * (fx->iterations ? fx->iterations : 1);
        if (run >= total) return false;
        elapsed = (uint32_t)(run % fx->duration);
    } else {
        elapsed = (uint32_t)run;
    }
    if (elapsed_out) *elapsed_out = elapsed;
    return true;
}

static int32_t periodic_value(const g923_effect *fx, uint32_t elapsed_us) {
    if (fx->periodic_period == 0) return fx->periodic_offset;
    double phase = (double)fx->periodic_phase / 36000.0;         /* 0..1 */
    double t = fmod((double)elapsed_us / (double)fx->periodic_period + phase, 1.0);
    double s; /* -1..1 */
    switch (fx->wave) {
    case G923_WAVE_SQUARE:   s = (t < 0.5) ? 1.0 : -1.0; break;
    case G923_WAVE_TRIANGLE: s = (t < 0.5) ? (4.0 * t - 1.0) : (3.0 - 4.0 * t); break;
    case G923_WAVE_SAWUP:    s = 2.0 * t - 1.0; break;
    case G923_WAVE_SAWDOWN:  s = 1.0 - 2.0 * t; break;
    case G923_WAVE_SINE:
    default:                 s = sin(2.0 * M_PI * t); break;
    }
    return fx->periodic_offset + (int32_t)(s * (double)fx->periodic_magnitude);
}

void g923_engine_tick(g923_engine *e, uint64_t now_us, g923_slot_forces *out) {
    memset(out, 0, sizeof(*out));
    if (e->paused || !e->actuators_on) return;

    int64_t constant_sum = 0;   /* DI scale, signed */
    bool any_output = false;

    for (int i = 1; i < G923_MAX_EFFECTS; i++) {
        g923_effect *fx = &e->effects[i];
        uint32_t elapsed;
        if (!effect_active(fx, now_us, &elapsed)) continue;

        int32_t base;
        switch (fx->kind) {
        case G923_FX_CONSTANT:
            base = fx->constant_level;
            break;
        case G923_FX_RAMP: {
            double frac = (fx->duration && fx->duration != G923_INFINITE)
                          ? (double)elapsed / (double)fx->duration : 0.0;
            base = fx->ramp_start + (int32_t)(frac * (fx->ramp_end - fx->ramp_start));
            break;
        }
        case G923_FX_PERIODIC:
            base = periodic_value(fx, elapsed);
            break;
        default:
            continue; /* conditions handled below */
        }
        int32_t lvl = apply_gains(e, fx, base, elapsed);
        constant_sum += (int64_t)lvl * direction_sign(fx);
        any_output = true;
    }

    if (any_output) {
        out->has_constant = true;
        if (constant_sum >  G923_NOMINAL_MAX) constant_sum =  G923_NOMINAL_MAX;
        if (constant_sum < -G923_NOMINAL_MAX) constant_sum = -G923_NOMINAL_MAX;
        out->constant_level = (int32_t)(constant_sum * 0x7fff / G923_NOMINAL_MAX);
    }

    for (int i = 1; i < G923_MAX_EFFECTS; i++) {
        g923_effect *fx = &e->effects[i];
        uint32_t elapsed;
        if (!effect_active(fx, now_us, &elapsed)) continue;
        if (fx->kind < G923_FX_SPRING) continue;

        int32_t kp = (int32_t)((int64_t)fx->cond_pos_coeff * fx->gain / G923_NOMINAL_MAX);
        int32_t kn = (int32_t)((int64_t)fx->cond_neg_coeff * fx->gain / G923_NOMINAL_MAX);
        kp = (int32_t)((int64_t)kp * e->device_gain / G923_NOMINAL_MAX);
        kn = (int32_t)((int64_t)kn * e->device_gain / G923_NOMINAL_MAX);
        int32_t k1 = (int32_t)((int64_t)kp * 0x7fff / G923_NOMINAL_MAX);
        int32_t k2 = (int32_t)((int64_t)kn * 0x7fff / G923_NOMINAL_MAX);
        int32_t clip = fx->cond_pos_sat > 0 ? (int32_t)((int64_t)fx->cond_pos_sat * 0xffff / G923_NOMINAL_MAX) : 0xffff;
        int32_t d1 = (int32_t)((int64_t)(fx->cond_center - fx->cond_deadband) * 0x7fff / G923_NOMINAL_MAX);
        int32_t d2 = (int32_t)((int64_t)(fx->cond_center + fx->cond_deadband) * 0x7fff / G923_NOMINAL_MAX);

        switch (fx->kind) {
        case G923_FX_SPRING:
            out->has_spring = true;
            out->spring_k1 = k1; out->spring_k2 = k2;
            out->spring_d1 = d1; out->spring_d2 = d2; out->spring_clip = clip;
            break;
        case G923_FX_DAMPER:
        case G923_FX_INERTIA:
            out->has_damper = true;
            out->damper_k1 = k1; out->damper_k2 = k2; out->damper_clip = clip;
            break;
        case G923_FX_FRICTION:
            out->has_friction = true;
            out->friction_k1 = k1; out->friction_k2 = k2; out->friction_clip = clip;
            break;
        default: break;
        }
    }
}

int g923_encode_slots(const g923_slot_forces *f, uint8_t *cmds) {
    int n = 0;
    if (f->has_constant) { g923_cmd_constant(cmds + n*G923_CMD_LEN, 0, f->constant_level); n++; }
    if (f->has_spring)   { g923_cmd_spring  (cmds + n*G923_CMD_LEN, 1, f->spring_k1, f->spring_k2, f->spring_d1, f->spring_d2, f->spring_clip); n++; }
    if (f->has_damper)   { g923_cmd_damper  (cmds + n*G923_CMD_LEN, 2, f->damper_k1, f->damper_k2, f->damper_clip); n++; }
    if (f->has_friction) { g923_cmd_friction(cmds + n*G923_CMD_LEN, 3, f->friction_k1, f->friction_k2, f->friction_clip); n++; }
    return n;
}
