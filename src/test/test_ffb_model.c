/*
 * test_ffb_model.c — hardware-free tests for the telemetry->classic-FFB model.
 * Checks the behavioral invariants (not exact feel, which needs a wheel).
 */
#include <stdio.h>
#include <string.h>
#include "g923_ffb_model.h"
#include "g923_effects.h"

static int fails = 0, passes = 0;
static void ck(const char *n, int c) { if (c) { passes++; printf("  PASS %s\n", n); } else { fails++; printf("  FAIL %s\n", n); } }

static g923_telemetry_shm mk(float speed_ms, float rpm, float rpmmax, int conn) {
    g923_telemetry_shm s; memset(&s, 0, sizeof s);
    s.magic = G923_TELEMETRY_MAGIC; s.version = G923_TELEMETRY_VERSION;
    s.connected = conn; s.paused = 0;
    s.speed_ms = speed_ms; s.engine_rpm = rpm; s.engine_rpm_max = rpmmax;
    return s;
}

int main(void) {
    printf("=== telemetry -> classic FFB model ===\n");
    g923_ffb_model m; g923_ffb_model_init(&m, NULL);
    g923_slot_forces f;

    /* not connected -> no force */
    g923_telemetry_shm off = mk(20, 1500, 2500, 0);
    g923_ffb_model_step(&m, &off, 0.0f, 0.01, &f);
    ck("disconnected -> silent", !f.has_constant && !f.has_spring);

    /* paused -> no force */
    g923_telemetry_shm pz = mk(20, 1500, 2500, 1); pz.paused = 1;
    g923_ffb_model_step(&m, &pz, 0.0f, 0.01, &f);
    ck("paused -> silent", !f.has_constant && !f.has_spring);

    /* stationary connected -> ~no centering spring */
    g923_telemetry_shm stop = mk(0, 800, 2500, 1);
    g923_ffb_model_step(&m, &stop, 0.0f, 0.01, &f);
    ck("stationary -> spring ~0", !f.has_spring || f.spring_k1 == 0);

    /* faster -> stronger centering spring */
    g923_ffb_model_init(&m, NULL);
    g923_telemetry_shm slow = mk(5, 1200, 2500, 1);   /* 18 km/h */
    g923_telemetry_shm fast = mk(25, 1200, 2500, 1);  /* 90 km/h */
    g923_slot_forces fs, ff;
    g923_ffb_model_step(&m, &slow, 0, 0.01, &fs);
    g923_ffb_model_step(&m, &fast, 0, 0.01, &ff);
    ck("spring present at speed", ff.has_spring && ff.spring_k1 > 0);
    ck("faster => stronger centering", ff.spring_k1 > fs.spring_k1);

    /* higher RPM -> larger rumble amplitude (sample the peak over a cycle) */
    g923_ffb_model_init(&m, NULL);
    int32_t peak_lo = 0, peak_hi = 0;
    g923_telemetry_shm lo = mk(10, 700, 2500, 1);
    g923_telemetry_shm hi = mk(10, 2400, 2500, 1);
    for (int i = 0; i < 400; i++) { g923_ffb_model_step(&m, &lo, 0, 0.005, &f); if (f.has_constant && abs(f.constant_level) > peak_lo) peak_lo = abs(f.constant_level); }
    g923_ffb_model_init(&m, NULL);
    for (int i = 0; i < 400; i++) { g923_ffb_model_step(&m, &hi, 0, 0.005, &f); if (f.has_constant && abs(f.constant_level) > peak_hi) peak_hi = abs(f.constant_level); }
    ck("higher RPM => stronger rumble", peak_hi > peak_lo);

    /* roughness increases constant peak */
    g923_ffb_model_init(&m, NULL);
    int32_t peak_smooth = 0, peak_rough = 0;
    g923_telemetry_shm cruise = mk(20, 1500, 2500, 1);
    for (int i = 0; i < 400; i++) { g923_ffb_model_step(&m, &cruise, 0.0f, 0.005, &f); if (f.has_constant && abs(f.constant_level) > peak_smooth) peak_smooth = abs(f.constant_level); }
    g923_ffb_model_init(&m, NULL);
    for (int i = 0; i < 400; i++) { g923_ffb_model_step(&m, &cruise, 1.0f, 0.005, &f); if (f.has_constant && abs(f.constant_level) > peak_rough) peak_rough = abs(f.constant_level); }
    ck("rough road => bigger jolts", peak_rough > peak_smooth);

    /* forces encode to valid classic commands */
    g923_ffb_model_step(&m, &fast, 0.5f, 0.01, &f);
    uint8_t cmds[4 * G923_CMD_LEN];
    int n = g923_encode_slots(&f, cmds);
    ck("encodes >=1 command at speed", n >= 1);

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
