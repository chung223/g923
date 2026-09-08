/*
 * g923_scs_plugin — SCS telemetry plugin for Euro Truck Simulator 2 / American
 * Truck Simulator. The game loads this shared library from its plugins folder;
 * it subscribes to a few telemetry channels and publishes them into the POSIX
 * shared-memory block defined in g923_telemetry_shm.h, which g923d reads to
 * drive the (v2) TrueForce rumble layer.
 *
 * BUILD: this file needs the official SCS SDK headers (shipped in the game's
 * `sdk/` folder or from the SCS modding wiki). It is NOT built by `make` — see
 * src/scs-plugin/README.md for the build+install command:
 *   make scs-plugin SCS_SDK=/path/to/scs_sdk
 *
 * Only the parts touching the SDK live here; the shared-memory layout and the
 * reader are plain, testable C in src/common/.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"

#include "g923_telemetry_shm.h"

static g923_telemetry_shm *g_shm = NULL;
static int g_fd = -1;
static scs_log_t g_log = NULL;

static bool shm_create(void) {
    g_fd = shm_open(G923_TELEMETRY_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (g_fd < 0) return false;
    if (ftruncate(g_fd, sizeof(g923_telemetry_shm)) != 0) { close(g_fd); g_fd = -1; return false; }
    g_shm = (g923_telemetry_shm *)mmap(NULL, sizeof(g923_telemetry_shm),
                                       PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_shm == MAP_FAILED) { g_shm = NULL; close(g_fd); g_fd = -1; return false; }
    memset(g_shm, 0, sizeof(*g_shm));
    g_shm->magic = G923_TELEMETRY_MAGIC;
    g_shm->version = G923_TELEMETRY_VERSION;
    return true;
}
static void shm_destroy(void) {
    if (g_shm && g_shm != MAP_FAILED) { g_shm->connected = 0; munmap(g_shm, sizeof(*g_shm)); }
    if (g_fd >= 0) close(g_fd);
    g_shm = NULL; g_fd = -1;
    shm_unlink(G923_TELEMETRY_SHM_NAME);
}

/* ---- channel callbacks ---- */
static SCSAPI_VOID chan_float(const scs_string_t name, const scs_u32_t index,
                              const scs_value_t *const value, const scs_context_t context) {
    (void)name; (void)index;
    if (!g_shm || !value || value->type != SCS_VALUE_TYPE_float) return;
    *(float *)context = value->value_float.value;
}
static SCSAPI_VOID chan_s32(const scs_string_t name, const scs_u32_t index,
                            const scs_value_t *const value, const scs_context_t context) {
    (void)name; (void)index;
    if (!g_shm || !value || value->type != SCS_VALUE_TYPE_s32) return;
    *(int32_t *)context = value->value_s32.value;
}
static SCSAPI_VOID chan_susp(const scs_string_t name, const scs_u32_t index,
                             const scs_value_t *const value, const scs_context_t context) {
    (void)name; (void)context;
    if (!g_shm || index >= G923_TELEMETRY_MAX_WHEELS) return;
    if (value && value->type == SCS_VALUE_TYPE_float)
        g_shm->wheel_susp_defl[index] = value->value_float.value;
}
static SCSAPI_VOID chan_onground(const scs_string_t name, const scs_u32_t index,
                                 const scs_value_t *const value, const scs_context_t context) {
    (void)name; (void)context;
    if (!g_shm || index >= G923_TELEMETRY_MAX_WHEELS) return;
    if (value && value->type == SCS_VALUE_TYPE_bool)
        g_shm->wheel_on_ground[index] = value->value_bool.value ? 1u : 0u;
}

/* ---- events ---- */
static SCSAPI_VOID ev_frame_start(const scs_event_t event, const void *const info, const scs_context_t ctx) {
    (void)event; (void)ctx;
    if (!g_shm) return;
    const scs_telemetry_frame_start_t *fs = (const scs_telemetry_frame_start_t *)info;
    g923_shm_write_begin(g_shm);
    g_shm->connected = 1;
    if (fs) g_shm->game_time_s = (double)fs->render_time.value / 1000000.0;
}
static SCSAPI_VOID ev_frame_end(const scs_event_t event, const void *const info, const scs_context_t ctx) {
    (void)event; (void)info; (void)ctx;
    if (g_shm) g923_shm_write_end(g_shm);
}
static SCSAPI_VOID ev_paused(const scs_event_t event, const void *const info, const scs_context_t ctx) {
    (void)info; (void)ctx;
    if (g_shm) g_shm->paused = (event == SCS_TELEMETRY_EVENT_paused) ? 1u : 0u;
}
static SCSAPI_VOID ev_config(const scs_event_t event, const void *const info, const scs_context_t ctx) {
    (void)event; (void)ctx;
    if (!g_shm) return;
    const scs_telemetry_configuration_t *cfg = (const scs_telemetry_configuration_t *)info;
    if (!cfg || !cfg->id) return;
    if (strcmp(cfg->id, SCS_TELEMETRY_CONFIG_truck) != 0) return;
    for (const scs_named_value_t *a = cfg->attributes; a && a->name; a++) {
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_rpm_limit) == 0 &&
            a->value.type == SCS_VALUE_TYPE_float)
            g_shm->engine_rpm_max = a->value.value_float.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) == 0 &&
                 a->value.type == SCS_VALUE_TYPE_u32)
            g_shm->wheel_count = a->value.value_u32.value;
    }
}

/* ---- entry points ---- */
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params) {
    if (version != SCS_TELEMETRY_VERSION_1_00) return SCS_RESULT_unsupported;
    const scs_telemetry_init_params_v100_t *const v =
        (const scs_telemetry_init_params_v100_t *)params;
    g_log = v->common.log;

    if (!shm_create()) {
        if (g_log) g_log(SCS_LOG_TYPE_error, "g923: could not create shared memory");
        return SCS_RESULT_generic_error;
    }

    v->register_for_event(SCS_TELEMETRY_EVENT_frame_start, ev_frame_start, NULL);
    v->register_for_event(SCS_TELEMETRY_EVENT_frame_end,   ev_frame_end,   NULL);
    v->register_for_event(SCS_TELEMETRY_EVENT_paused,      ev_paused,      NULL);
    v->register_for_event(SCS_TELEMETRY_EVENT_started,     ev_paused,      NULL);
    v->register_for_event(SCS_TELEMETRY_EVENT_configuration, ev_config,    NULL);

    v->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, SCS_U32_NIL,
        SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, chan_float, &g_shm->engine_rpm);
    v->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL,
        SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, chan_float, &g_shm->speed_ms);
    v->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_U32_NIL,
        SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none, chan_s32, &g_shm->gear);
    v->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_susp_deflection, SCS_U32_NIL,
        SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_each_frame, chan_susp, NULL);
    v->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_on_ground, SCS_U32_NIL,
        SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none, chan_onground, NULL);

    if (g_log) g_log(SCS_LOG_TYPE_message, "g923: telemetry plugin initialized");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    shm_destroy();
    g_log = NULL;
}
