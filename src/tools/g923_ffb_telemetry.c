/*
 * g923_ffb_telemetry — native telemetry-driven force feedback for ETS2/ATS (v2).
 *
 * Native macOS ETS2 emits no force feedback of its own, but it loads telemetry
 * plugins. This runner reads the game telemetry our SCS plugin publishes and
 * drives the G923 DIRECTLY via IOKit HID with classic Logitech FFB commands
 * (centering spring + engine rumble + road/bump jolts). It does not touch
 * ForceFeedback.framework and does not need the game to support FFB.
 *
 *   g923_ffb_telemetry [--rate HZ] [--range DEG] [--gain G] [--invert] [--once]
 *
 * Needs: the wheel attached (native mode) and the SCS telemetry plugin running
 * inside the game. If either is missing it says so and idles/exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include "g923_find.h"
#include "g923_hid.h"
#include "g923_protocol.h"
#include "g923_effects.h"
#include "g923_telemetry_reader.h"
#include "g923_ffb_model.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void zero_wheel(g923_hid *h) {
    uint8_t c[G923_CMD_LEN];
    g923_cmd_constant(c, 0, 0); g923_hid_send(h, 0, c);
    for (int s = 1; s < 4; s++) { g923_cmd_stop(c, s); g923_hid_send(h, 0, c); }
}

int main(int argc, char **argv) {
    int rate = 100, range = 900, once = 0;
    float gain = 1.0f; int invert = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i+1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--range") && i+1 < argc) range = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i+1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--invert")) invert = 1;
        else if (!strcmp(argv[i], "--once")) once = 1;
        else { fprintf(stderr, "usage: g923_ffb_telemetry [--rate HZ] [--range DEG] [--gain G] [--invert] [--once]\n"); return 2; }
    }
    if (rate < 10) rate = 10; if (rate > 500) rate = 500;

    /* wheel */
    uint16_t vid, pid;
    io_service_t svc = g923_find_wheel(&vid, &pid);
    if (svc == IO_OBJECT_NULL) { fprintf(stderr, "no supported wheel attached.\n"); return 1; }
    g923_hid hid;
    if (!g923_hid_wrap(&hid, svc) || !g923_hid_open(&hid)) {
        fprintf(stderr, "could not open the wheel for output.\n"); IOObjectRelease(svc); return 1;
    }
    IOObjectRelease(svc);
    printf("wheel: %s (0x%04x:0x%04x)\n", g923_wheel_name(pid), vid, pid);
    { uint8_t c[G923_CMD_LEN];
      g923_cmd_autocenter_off(c); g923_hid_send(&hid, 0, c);
      g923_cmd_set_range(c, (uint16_t)range); g923_hid_send(&hid, 0, c);
      g923_cmd_timeloop(c, false); g923_hid_send(&hid, 0, c); }

    /* telemetry */
    g923_telemetry_reader rd;
    int have_tel = g923_telemetry_reader_open(&rd);
    if (!have_tel)
        fprintf(stderr, "note: telemetry not found yet (install the SCS plugin and launch ETS2/ATS). Will keep trying.\n");

    /* model */
    g923_ffb_config cfg; g923_ffb_config_defaults(&cfg);
    cfg.centering_gain *= gain; cfg.rumble_gain *= gain; cfg.bump_gain *= gain;
    cfg.invert = invert;
    g923_ffb_model model; g923_ffb_model_init(&model, &cfg);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    const useconds_t period = (useconds_t)(1000000 / rate);
    const double dt = 1.0 / rate;
    printf("driving telemetry FFB at %d Hz (range %d, gain %.2f%s). Ctrl-C to stop.\n",
           rate, range, gain, invert ? ", inverted" : "");

    int last_conn = -1;
    do {
        if (!have_tel) have_tel = g923_telemetry_reader_open(&rd);
        g923_telemetry_shm snap;
        g923_slot_forces f; memset(&f, 0, sizeof f);
        if (have_tel && g923_telemetry_reader_sample(&rd, &snap)) {
            float rough = g923_telemetry_road_roughness(&rd, &snap);
            g923_ffb_model_step(&model, &snap, rough, dt, &f);
            if ((int)snap.connected != last_conn) {
                printf("telemetry %s\n", snap.connected ? "connected" : "idle");
                last_conn = (int)snap.connected;
            }
        }
        uint8_t cmds[4 * G923_CMD_LEN];
        int n = g923_encode_slots(&f, cmds);
        for (int i = 0; i < n; i++) g923_hid_send(&hid, 0, cmds + i * G923_CMD_LEN);
        if (!f.has_constant) { uint8_t z[G923_CMD_LEN]; g923_cmd_constant(z, 0, 0); g923_hid_send(&hid, 0, z); }
        if (!f.has_spring)   { uint8_t z[G923_CMD_LEN]; g923_cmd_stop(z, 1); g923_hid_send(&hid, 0, z); }
        if (once) break;
        usleep(period);
    } while (!g_stop);

    zero_wheel(&hid);
    if (have_tel) g923_telemetry_reader_close(&rd);
    g923_hid_close(&hid);
    printf("\nstopped; wheel forces cleared.\n");
    return 0;
}
