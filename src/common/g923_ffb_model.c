/* g923_ffb_model.c — see g923_ffb_model.h. v2. */
#include "g923_ffb_model.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define S16_MAX 0x7fff

void g923_ffb_config_defaults(g923_ffb_config *cfg) {
    cfg->centering_gain     = 0.6f;
    cfg->centering_speed_ref = 60.0f;  /* full centering by ~60 km/h */
    cfg->rumble_gain        = 0.15f;
    cfg->bump_gain          = 0.5f;
    cfg->invert             = false;
}

void g923_ffb_model_init(g923_ffb_model *m, const g923_ffb_config *cfg) {
    memset(m, 0, sizeof(*m));
    if (cfg) m->cfg = *cfg; else g923_ffb_config_defaults(&m->cfg);
    m->phase = 0.0;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void g923_ffb_model_step(g923_ffb_model *m, const g923_telemetry_shm *snap,
                         float roughness, double dt_s, g923_slot_forces *out) {
    memset(out, 0, sizeof(*out));
    if (!snap || !snap->connected || snap->paused) return;
    if (dt_s <= 0) dt_s = 0.01;

    float speed_kph = snap->speed_ms * 3.6f;
    float aspeed = speed_kph < 0 ? -speed_kph : speed_kph;
    float rpm_frac = 0.0f;
    if (snap->engine_rpm_max > 1.0f)
        rpm_frac = clampf(snap->engine_rpm / snap->engine_rpm_max, 0.0f, 1.0f);
    roughness = clampf(roughness, 0.0f, 1.0f);

    /* ---- centering spring (slot 1) : grows with speed ---- */
    float speed_frac = clampf(aspeed / (m->cfg.centering_speed_ref > 1 ? m->cfg.centering_speed_ref : 60.0f), 0.0f, 1.0f);
    int32_t k = (int32_t)(m->cfg.centering_gain * speed_frac * S16_MAX);
    if (k > 0) {
        out->has_spring = true;
        out->spring_k1 = k;
        out->spring_k2 = k;
        out->spring_d1 = 0;
        out->spring_d2 = 0;
        out->spring_clip = 0xffff;
    }

    /* ---- engine rumble + road/bump (constant slot 0) ---- */
    /* rumble frequency rises with RPM (8..48 Hz); coarse on a ~100 Hz channel
     * but audible as a low hum. */
    double freq = 8.0 + 40.0 * rpm_frac;
    m->phase += 2.0 * M_PI * freq * dt_s;
    if (m->phase > 2.0 * M_PI * 1e6) m->phase = fmod(m->phase, 2.0 * M_PI);

    float rumble_amp = m->cfg.rumble_gain * (0.3f + 0.7f * rpm_frac);
    float rumble = rumble_amp * (float)sin(m->phase);

    /* road/bump: roughness modulates a faster component; scales with speed so a
     * jolt at speed hits harder. */
    float bump_amp = m->cfg.bump_gain * roughness * (0.3f + 0.7f * speed_frac);
    float bump = bump_amp * (float)sin(m->phase * 3.3);

    float total = rumble + bump;          /* -~1..1 */
    total = clampf(total, -1.0f, 1.0f);
    int32_t level = (int32_t)(total * S16_MAX);
    if (m->cfg.invert) level = -level;

    if (level != 0) {
        out->has_constant = true;
        out->constant_level = level;
    }
}
