/* g923_telemetry_reader.c — see g923_telemetry_reader.h. v2. */
#include "g923_telemetry_reader.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

bool g923_telemetry_reader_open(g923_telemetry_reader *r) {
    memset(r, 0, sizeof(*r));
    r->fd = shm_open(G923_TELEMETRY_SHM_NAME, O_RDONLY, 0);
    if (r->fd < 0) return false;
    r->map_len = sizeof(g923_telemetry_shm);
    r->map = mmap(NULL, r->map_len, PROT_READ, MAP_SHARED, r->fd, 0);
    if (r->map == MAP_FAILED) { close(r->fd); r->fd = -1; r->map = NULL; return false; }
    return true;
}

void g923_telemetry_reader_close(g923_telemetry_reader *r) {
    if (r->map && r->map != MAP_FAILED) munmap(r->map, r->map_len);
    if (r->fd >= 0) close(r->fd);
    r->map = NULL; r->fd = -1;
}

bool g923_telemetry_reader_sample(g923_telemetry_reader *r, g923_telemetry_shm *out) {
    if (!r->map) return false;
    return g923_shm_read((const g923_telemetry_shm *)r->map, out);
}

float g923_telemetry_road_roughness(g923_telemetry_reader *r, const g923_telemetry_shm *cur) {
    float rough = 0.0f;
    if (r->have_prev) {
        uint32_t n = cur->wheel_count;
        if (n > G923_TELEMETRY_MAX_WHEELS) n = G923_TELEMETRY_MAX_WHEELS;
        float acc = 0.0f; uint32_t used = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!cur->wheel_on_ground[i]) continue;
            float d = fabsf(cur->wheel_susp_defl[i] - r->prev.wheel_susp_defl[i]);
            acc += d; used++;
        }
        if (used) {
            /* ~2 cm of per-sample deflection change ≈ very rough; clamp to 0..1 */
            rough = (acc / used) / 0.02f;
            if (rough > 1.0f) rough = 1.0f;
            if (rough < 0.0f) rough = 0.0f;
        }
    }
    r->prev = *cur;
    r->have_prev = true;
    return rough;
}

void g923_telemetry_to_tf(const g923_telemetry_shm *snap, float roughness,
                          g923_tf_telemetry_ctx *tf) {
    tf->engine_rpm = snap->engine_rpm;
    tf->rpm_max = snap->engine_rpm_max > 0 ? snap->engine_rpm_max : 2500.0f;
    tf->speed_kph = snap->speed_ms * 3.6f;
    tf->road_roughness = roughness;
    /* leave tf->phase running; the source advances it */
}
