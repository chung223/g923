/*
 * g923_trueforce.h — TrueForce haptic stream (v2, EXPERIMENTAL).
 *
 * STATUS: v2 / not enabled in the shipping driver. The G923's TrueForce channel
 * is a separate audio-rate haptic stream on HID interface 2 (vendor usage page
 * 0xFFFD, usage 0xFD01), NOT the classic 7-byte FFB protocol. This module lets
 * us *synthesize our own* haptics onto that channel (from telemetry or by
 * mirroring the classic force). It can NOT reproduce a Windows game's own
 * TrueForce content — that comes only from Logitech's signed Windows SDK via a
 * G HUB named pipe, which does not exist on macOS.
 *
 * The wire format below is MEDIUM confidence, reconstructed from community
 * Windows captures (Trueforce-For-All, mescon). Every value marked (UNVERIFIED)
 * must be confirmed against a real wheel with tools/g923_probe_if2 before this
 * is trusted. Do not enable in production until then.
 *
 * Primary use case for this project: Euro Truck Simulator 2 / American Truck
 * Simulator, whose official SCS telemetry SDK is an ideal sample source (engine
 * RPM, wheel slip, road surface) for a self-synthesized rumble layer.
 */
#ifndef G923_TRUEFORCE_H
#define G923_TRUEFORCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "g923_hid.h"

/* --- interface identity (confirmed from descriptor dumps) --- */
#define G923_TF_USAGE_PAGE   0xFFFDu
#define G923_TF_USAGE        0xFD01u
#define G923_TF_REPORT_ID    0x01u

/* --- stream format (UNVERIFIED — confirm with g923_probe_if2) --- */
#define G923_TF_PACKET_LEN       64   /* bytes incl. report id (UNVERIFIED) */
#define G923_TF_WINDOW_SAMPLES   13   /* rolling window depth (UNVERIFIED) */
#define G923_TF_NEW_PER_PACKET   4    /* new 16-bit samples per packet (UNVERIFIED) */
#define G923_TF_SILENCE          0x8000u /* mid-scale == no force (UNVERIFIED) */
#define G923_TF_RATE_HZ_DEFAULT  500  /* 250..1000 observed (UNVERIFIED) */

typedef struct {
    g923_hid hid;              /* opened on the IF2 IOHIDDevice */
    bool     open;
    int16_t  window[G923_TF_WINDOW_SAMPLES]; /* rolling context, centered at silence */
    uint32_t rate_hz;
} g923_trueforce;

/* A sample source: fills `out` with `count` signed samples in [-32767,32767]
 * (0 == no force). Return the number produced (may be < count on underrun;
 * caller fills the rest with silence). ctx is opaque. */
typedef int (*g923_tf_source_fn)(void *ctx, int16_t *out, int count);

/* Locate + open the TrueForce interface of an attached wheel. Returns false if
 * the wheel or the IF2 interface is not present. */
bool g923_tf_open(g923_trueforce *tf);
void g923_tf_close(g923_trueforce *tf);

/* Send the (UNVERIFIED) init handshake for the stream. Returns false on error.
 * No-op-safe: logs what it would send. */
bool g923_tf_init(g923_trueforce *tf);

/* Build one packet from the current rolling window + `new_samples` new samples
 * (exactly G923_TF_NEW_PER_PACKET). Writes G923_TF_PACKET_LEN bytes into `pkt`
 * (pkt[0] = report id). This is pure/encoding-only, so it is unit-testable. */
void g923_tf_encode_packet(g923_trueforce *tf, const int16_t *new_samples,
                           uint8_t pkt[G923_TF_PACKET_LEN]);

/* Push one packet's worth of new samples to the wheel. */
bool g923_tf_push(g923_trueforce *tf, const int16_t new_samples[G923_TF_NEW_PER_PACKET]);

/* Send a silence packet (all samples == silence). */
bool g923_tf_silence(g923_trueforce *tf);

/* Run the stream loop, pulling samples from `src` at the configured rate until
 * `*keep_running` becomes false. Blocking; run on its own thread. */
void g923_tf_run(g923_trueforce *tf, g923_tf_source_fn src, void *ctx,
                 volatile bool *keep_running);

/* --- built-in sample sources (stubs; v2) --- */

/* Mirror a classic constant-force level into a low-frequency rumble. `level_q15`
 * is the current signed classic force (-32767..32767); updated by the caller. */
typedef struct { volatile int32_t level_q15; uint32_t phase; } g923_tf_mirror_ctx;
int  g923_tf_source_mirror(void *ctx, int16_t *out, int count);

/* Telemetry-driven source (e.g. ETS2/ATS SCS SDK). The concrete telemetry
 * reader is out of scope here; this stub returns silence until wired up. */
typedef struct {
    volatile float engine_rpm;     /* set by a telemetry reader thread */
    volatile float rpm_max;
    volatile float speed_kph;
    volatile float road_roughness; /* 0..1 */
    uint32_t phase;
} g923_tf_telemetry_ctx;
int  g923_tf_source_telemetry(void *ctx, int16_t *out, int count);

#endif /* G923_TRUEFORCE_H */
