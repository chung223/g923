/*
 * g923_telemetry_shm.h — the shared-memory contract between the SCS telemetry
 * plugin (loaded inside ETS2/ATS) and our reader (g923d / tools).
 *
 * The plugin writes this struct into a POSIX shared-memory segment; any of our
 * processes maps it read-only and samples it. A seqlock makes reads consistent
 * without locking the writer. This header is shared by both sides, so it has no
 * dependencies beyond the C standard library.
 *
 * v2 / feeds the TrueForce layer. Nothing in the shipping FFB path needs it.
 */
#ifndef G923_TELEMETRY_SHM_H
#define G923_TELEMETRY_SHM_H

#include <stdint.h>
#include <stdbool.h>

#define G923_TELEMETRY_SHM_NAME  "/g923_telemetry"   /* shm_open name */
#define G923_TELEMETRY_MAGIC     0x47393233u         /* 'G923' */
#define G923_TELEMETRY_VERSION   1u
#define G923_TELEMETRY_MAX_WHEELS 8

typedef struct {
    uint32_t magic;        /* == G923_TELEMETRY_MAGIC once initialized */
    uint32_t version;      /* == G923_TELEMETRY_VERSION */
    volatile uint32_t seq; /* seqlock: odd while writing, even when stable */

    uint32_t connected;    /* 1 while the game/telemetry is active */
    uint32_t paused;       /* 1 while the game is paused */

    /* live truck state (SI-ish units from the SCS SDK) */
    float engine_rpm;      /* current RPM */
    float engine_rpm_max;  /* rpm_limit from truck config */
    float speed_ms;        /* m/s (SCS gives m/s) */
    int32_t gear;          /* current gear (negative = reverse) */

    uint32_t wheel_count;
    float wheel_susp_defl[G923_TELEMETRY_MAX_WHEELS]; /* suspension deflection, m */
    uint32_t wheel_on_ground[G923_TELEMETRY_MAX_WHEELS];

    double game_time_s;    /* monotonically increasing game timestamp */
} g923_telemetry_shm;

/* ---- writer side (used by the SCS plugin) ---- */
static inline void g923_shm_write_begin(g923_telemetry_shm *s) {
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);   /* -> odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static inline void g923_shm_write_end(g923_telemetry_shm *s) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);   /* -> even */
}

/* ---- reader side ---- */
/* Consistent snapshot read. Returns false if the segment is not a valid,
 * initialized telemetry block or if it could not be read stably. */
static inline bool g923_shm_read(const g923_telemetry_shm *s, g923_telemetry_shm *out) {
    if (!s) return false;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u) continue;                 /* writer mid-update */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = *s;                             /* copy body */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1) {
            return out->magic == G923_TELEMETRY_MAGIC &&
                   out->version == G923_TELEMETRY_VERSION;
        }
    }
    return false;
}

#endif /* G923_TELEMETRY_SHM_H */
