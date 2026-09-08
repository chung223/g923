/* g923_trueforce.c — see g923_trueforce.h. v2 / EXPERIMENTAL.
 * The packet layout and init handshake here are UNVERIFIED reconstructions from
 * community Windows captures; confirm with tools/g923_probe_if2 on real
 * hardware before enabling. Nothing in the shipping driver calls this yet. */
#include "g923_trueforce.h"
#include "g923_find.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TFLOG(fmt, ...) fprintf(stderr, "g923-trueforce[v2]: " fmt "\n", ##__VA_ARGS__)

bool g923_tf_open(g923_trueforce *tf) {
    memset(tf, 0, sizeof(*tf));
    tf->rate_hz = G923_TF_RATE_HZ_DEFAULT;
    for (int i = 0; i < G923_TF_WINDOW_SAMPLES; i++) tf->window[i] = 0;
    uint16_t vid = 0, pid = 0;
    io_service_t svc = g923_find_wheel_iface(G923_TF_USAGE_PAGE, G923_TF_USAGE, &vid, &pid);
    if (svc == IO_OBJECT_NULL) {
        TFLOG("TrueForce interface (usage %04x/%04x) not found; is the wheel in native mode?",
              G923_TF_USAGE_PAGE, G923_TF_USAGE);
        return false;
    }
    bool ok = g923_hid_wrap(&tf->hid, svc) && g923_hid_open(&tf->hid);
    IOObjectRelease(svc);
    if (!ok) { TFLOG("could not open TrueForce interface for output"); return false; }
    tf->open = true;
    TFLOG("opened TrueForce interface (vid %04x pid %04x, max_out=%u)",
          vid, pid, tf->hid.max_output_len);
    return true;
}

void g923_tf_close(g923_trueforce *tf) {
    if (tf->open) g923_hid_close(&tf->hid);
    tf->open = false;
}

bool g923_tf_init(g923_trueforce *tf) {
    if (!tf->open) return false;
    /* UNVERIFIED: community captures describe a multi-packet handshake (~68
     * packets, a "double init"). Until a real capture confirms the exact bytes,
     * we only prime the stream with silence. Replace this with the real
     * handshake once g923_probe_if2 has dumped it. */
    TFLOG("init: sending silence priming frames (real handshake UNVERIFIED)");
    for (int i = 0; i < 4; i++) if (!g923_tf_silence(tf)) return false;
    return true;
}

void g923_tf_encode_packet(g923_trueforce *tf, const int16_t *new_samples,
                           uint8_t pkt[G923_TF_PACKET_LEN]) {
    /* UNVERIFIED layout: byte0 = report id; then a rolling window of 16-bit LE
     * samples, oldest first, with the newest G923_TF_NEW_PER_PACKET appended.
     * Samples are mapped signed -> unsigned around G923_TF_SILENCE. */
    memset(pkt, 0, G923_TF_PACKET_LEN);
    pkt[0] = G923_TF_REPORT_ID;

    /* advance the rolling window */
    int keep = G923_TF_WINDOW_SAMPLES - G923_TF_NEW_PER_PACKET;
    if (keep < 0) keep = 0;
    memmove(tf->window, tf->window + G923_TF_NEW_PER_PACKET, (size_t)keep * sizeof(int16_t));
    for (int i = 0; i < G923_TF_NEW_PER_PACKET && (keep + i) < G923_TF_WINDOW_SAMPLES; i++)
        tf->window[keep + i] = new_samples[i];

    /* serialize window as u16 LE around silence, starting at byte 1 */
    int off = 1;
    for (int i = 0; i < G923_TF_WINDOW_SAMPLES; i++) {
        int32_t v = (int32_t)G923_TF_SILENCE + tf->window[i];
        if (v < 0) v = 0; if (v > 0xffff) v = 0xffff;
        if (off + 1 >= G923_TF_PACKET_LEN) break;
        pkt[off++] = (uint8_t)(v & 0xff);
        pkt[off++] = (uint8_t)((v >> 8) & 0xff);
    }
}

bool g923_tf_push(g923_trueforce *tf, const int16_t new_samples[G923_TF_NEW_PER_PACKET]) {
    if (!tf->open) return false;
    uint8_t pkt[G923_TF_PACKET_LEN];
    g923_tf_encode_packet(tf, new_samples, pkt);
    /* payload after the report id is PACKET_LEN-1 bytes at report id TF_REPORT_ID */
    return g923_hid_send_report(&tf->hid, G923_TF_REPORT_ID, pkt + 1, G923_TF_PACKET_LEN - 1);
}

bool g923_tf_silence(g923_trueforce *tf) {
    int16_t z[G923_TF_NEW_PER_PACKET];
    for (int i = 0; i < G923_TF_NEW_PER_PACKET; i++) z[i] = 0;
    return g923_tf_push(tf, z);
}

void g923_tf_run(g923_trueforce *tf, g923_tf_source_fn src, void *ctx,
                 volatile bool *keep_running) {
    if (!tf->open) return;
    const useconds_t period = (useconds_t)(1000000u / (tf->rate_hz ? tf->rate_hz : G923_TF_RATE_HZ_DEFAULT));
    int16_t samples[G923_TF_NEW_PER_PACKET];
    while (keep_running && *keep_running) {
        int n = src ? src(ctx, samples, G923_TF_NEW_PER_PACKET) : 0;
        for (int i = n; i < G923_TF_NEW_PER_PACKET; i++) samples[i] = 0; /* pad with silence */
        g923_tf_push(tf, samples);
        usleep(period);
    }
    g923_tf_silence(tf);
}

/* --- built-in sources (v2 stubs) --- */

int g923_tf_source_mirror(void *vctx, int16_t *out, int count) {
    g923_tf_mirror_ctx *c = (g923_tf_mirror_ctx *)vctx;
    /* Low-frequency rumble whose amplitude tracks the classic force magnitude.
     * Purely illustrative until tuned on hardware. */
    int32_t amp = c->level_q15; if (amp < 0) amp = -amp;
    for (int i = 0; i < count; i++) {
        double s = sin((double)c->phase * 2.0 * M_PI / 64.0);
        out[i] = (int16_t)((amp * s) / 4); /* scaled down; needs hardware tuning */
        c->phase++;
    }
    return count;
}

int g923_tf_source_telemetry(void *vctx, int16_t *out, int count) {
    g923_tf_telemetry_ctx *c = (g923_tf_telemetry_ctx *)vctx;
    /* Engine-RPM vibration + road roughness. The actual telemetry reader (e.g.
     * the SCS SDK shared-memory client for ETS2/ATS) fills c->engine_rpm etc.
     * on another thread. Until wired up these are 0 -> silence. */
    double rpm = c->engine_rpm, rmax = c->rpm_max > 0 ? c->rpm_max : 2500.0;
    double frac = rpm / rmax; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    double freq = 20.0 + frac * 60.0;            /* 20..80 Hz engine hum */
    double amp = 4000.0 * frac + 8000.0 * c->road_roughness;
    if (amp > 20000.0) amp = 20000.0;
    for (int i = 0; i < count; i++) {
        double t = (double)c->phase / 500.0;     /* assume ~500 Hz stream */
        double s = sin(2.0 * M_PI * freq * t);
        out[i] = (int16_t)(amp * s);
        c->phase++;
    }
    return count;
}
