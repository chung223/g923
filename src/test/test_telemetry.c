/*
 * test_telemetry.c — hardware-free round-trip test of the SCS telemetry
 * shared-memory contract: create the segment, write like the plugin would,
 * read it back through the reader, and check the derived values. If POSIX shm
 * is unavailable in the sandbox, it SKIPs rather than fails.
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "g923_telemetry_shm.h"
#include "g923_telemetry_reader.h"

static int fails = 0, passes = 0;
static void ck(const char *n, int cond) {
    if (cond) { passes++; printf("  PASS %s\n", n); }
    else { fails++; printf("  FAIL %s\n", n); }
}

int main(void) {
    printf("=== SCS telemetry shm round-trip ===\n");
    const char *name = "/g923_telemetry_test";
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { printf("  SKIP (POSIX shm unavailable in this environment)\n\n0 failed (skipped)\n"); return 0; }
    if (ftruncate(fd, sizeof(g923_telemetry_shm)) != 0) { printf("  SKIP (ftruncate)\n"); close(fd); shm_unlink(name); return 0; }
    g923_telemetry_shm *w = mmap(NULL, sizeof(*w), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) { printf("  SKIP (mmap)\n"); close(fd); shm_unlink(name); return 0; }

    /* writer (as the plugin would) */
    memset(w, 0, sizeof(*w));
    w->magic = G923_TELEMETRY_MAGIC; w->version = G923_TELEMETRY_VERSION;
    g923_shm_write_begin(w);
    w->connected = 1; w->engine_rpm = 1500.0f; w->engine_rpm_max = 2500.0f;
    w->speed_ms = 25.0f; w->gear = 8; w->wheel_count = 4;
    w->wheel_on_ground[0] = w->wheel_on_ground[1] = 1;
    w->wheel_susp_defl[0] = 0.00f; w->wheel_susp_defl[1] = 0.00f;
    g923_shm_write_end(w);

    /* reader (map the same name via the generic helper) */
    g923_telemetry_reader r; memset(&r, 0, sizeof r);
    r.fd = shm_open(name, O_RDONLY, 0);
    r.map_len = sizeof(g923_telemetry_shm);
    r.map = mmap(NULL, r.map_len, PROT_READ, MAP_SHARED, r.fd, 0);
    ck("reader mapped", r.map && r.map != MAP_FAILED);

    g923_telemetry_shm s;
    ck("sample ok", g923_telemetry_reader_sample(&r, &s));
    ck("rpm", s.engine_rpm == 1500.0f);
    ck("rpm_max", s.engine_rpm_max == 2500.0f);
    ck("gear", s.gear == 8);

    float rough1 = g923_telemetry_road_roughness(&r, &s); /* first: no prev -> 0 */
    ck("roughness zero on first sample", rough1 == 0.0f);

    /* second write: jolt the suspension */
    g923_shm_write_begin(w);
    w->wheel_susp_defl[0] = 0.02f; w->wheel_susp_defl[1] = 0.02f;
    g923_shm_write_end(w);
    g923_telemetry_reader_sample(&r, &s);
    float rough2 = g923_telemetry_road_roughness(&r, &s);
    ck("roughness rises after jolt", rough2 > 0.5f);

    /* bridge into TrueForce ctx */
    g923_tf_telemetry_ctx tf; memset(&tf, 0, sizeof tf);
    g923_telemetry_to_tf(&s, rough2, &tf);
    ck("tf rpm mapped", tf.engine_rpm == s.engine_rpm);
    ck("tf speed kph", tf.speed_kph > 89.0f && tf.speed_kph < 91.0f); /* 25 m/s = 90 km/h */

    if (r.map) munmap(r.map, r.map_len);
    if (r.fd >= 0) close(r.fd);
    munmap(w, sizeof(*w)); close(fd); shm_unlink(name);

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
