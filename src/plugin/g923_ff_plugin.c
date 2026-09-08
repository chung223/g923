/*
 * g923_ff_plugin.c — macOS ForceFeedback.framework device plug-in for the
 * Logitech G923 (and G29-family) wheel.
 *
 * Implements IOForceFeedbackDeviceInterface (see IOForceFeedbackLib.h). The
 * framework loads this bundle into the *game's* process and calls the vtable
 * below when the game drives DirectInput-style force feedback. We translate
 * effects into classic Logitech HID output reports and send them straight to
 * the wheel via IOHIDDevice.
 *
 * Loading path: a companion daemon (g923d) injects this bundle into the wheel's
 * IOCFPlugInTypes so the framework finds us. See g923_inject.*.
 *
 * Build: clang -bundle ... -framework ForceFeedback -framework IOKit -framework CoreFoundation
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <ForceFeedback/ForceFeedback.h>
#include <ForceFeedback/IOForceFeedbackLib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>

#include "g923_effects.h"
#include "g923_hid.h"

/* Our CFPlugIn factory UUID (must match Info.plist CFPlugInFactories key).
 * 6E2A9F44-3C21-4E7A-9C4E-1A2B3C4D5E6F */
#define kG923FactoryID CFUUIDGetConstantUUIDWithBytes(NULL, \
    0x6E,0x2A,0x9F,0x44,0x3C,0x21,0x4E,0x7A,0x9C,0x4E,0x1A,0x2B,0x3C,0x4D,0x5E,0x6F)

#define DEFAULT_RANGE_DEG 900
#define TICK_HZ           100

#define LOGP(fmt, ...) os_log(OS_LOG_DEFAULT, "[G923FF] " fmt, ##__VA_ARGS__)

/* ---- instance ----------------------------------------------------------- */
typedef struct {
    IOCFPlugInInterface            *cfPlugInVtbl;   /* first: IOCFPlugInInterface** */
    IOForceFeedbackDeviceInterface *ffVtbl;
    CFUUIDRef  factoryID;
    UInt32     refCount;

    io_service_t service;
    g923_hid   hid;
    bool       hid_ready;

    g923_engine engine;
    pthread_mutex_t lock;

    /* map FFEffectDownloadID -> engine handle (1:1 here) */
    pthread_t  ticker;
    bool       ticker_run;
    uint64_t   epoch_us;
} G923Plugin;

static G923Plugin *fromCF(void *p) { return (G923Plugin *)p; }
static G923Plugin *fromFF(void *p) { return (G923Plugin *)((char *)p - offsetof(G923Plugin, ffVtbl)); }

/* monotonic microseconds */
#include <mach/mach_time.h>
static uint64_t now_us(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000ull;
}

/* ---- wheel I/O ---------------------------------------------------------- */
static void send_slot_forces(G923Plugin *s) {
    if (!s->hid_ready) return;
    g923_slot_forces f;
    g923_engine_tick(&s->engine, now_us(), &f);
    uint8_t cmds[4 * G923_CMD_LEN];
    int n = g923_encode_slots(&f, cmds);
    for (int i = 0; i < n; i++)
        g923_hid_send(&s->hid, 0, cmds + i * G923_CMD_LEN);
    /* if nothing active, make sure constant slot is zeroed */
    if (!f.has_constant) {
        uint8_t stop[G923_CMD_LEN];
        g923_cmd_constant(stop, 0, 0);
        g923_hid_send(&s->hid, 0, stop);
    }
}

static void *ticker_main(void *arg) {
    G923Plugin *s = (G923Plugin *)arg;
    const useconds_t period = 1000000 / TICK_HZ;
    while (s->ticker_run) {
        pthread_mutex_lock(&s->lock);
        if (s->hid_ready) send_slot_forces(s);
        pthread_mutex_unlock(&s->lock);
        usleep(period);
    }
    return NULL;
}

/* ---- effect translation ------------------------------------------------- */
static g923_effect_kind kind_for_uuid(CFUUIDRef t) {
    if (CFEqual(t, kFFEffectType_ConstantForce_ID)) return G923_FX_CONSTANT;
    if (CFEqual(t, kFFEffectType_RampForce_ID))     return G923_FX_RAMP;
    if (CFEqual(t, kFFEffectType_Square_ID) || CFEqual(t, kFFEffectType_Sine_ID) ||
        CFEqual(t, kFFEffectType_Triangle_ID) || CFEqual(t, kFFEffectType_SawtoothUp_ID) ||
        CFEqual(t, kFFEffectType_SawtoothDown_ID)) return G923_FX_PERIODIC;
    if (CFEqual(t, kFFEffectType_Spring_ID))   return G923_FX_SPRING;
    if (CFEqual(t, kFFEffectType_Damper_ID))   return G923_FX_DAMPER;
    if (CFEqual(t, kFFEffectType_Inertia_ID))  return G923_FX_INERTIA;
    if (CFEqual(t, kFFEffectType_Friction_ID)) return G923_FX_FRICTION;
    return G923_FX_NONE;
}

static g923_wave wave_for_uuid(CFUUIDRef t) {
    if (CFEqual(t, kFFEffectType_Square_ID))       return G923_WAVE_SQUARE;
    if (CFEqual(t, kFFEffectType_Triangle_ID))     return G923_WAVE_TRIANGLE;
    if (CFEqual(t, kFFEffectType_SawtoothUp_ID))   return G923_WAVE_SAWUP;
    if (CFEqual(t, kFFEffectType_SawtoothDown_ID)) return G923_WAVE_SAWDOWN;
    return G923_WAVE_SINE;
}

static void parse_effect(g923_effect *fx, CFUUIDRef type, FFEFFECT *e) {
    fx->kind = kind_for_uuid(type);
    fx->duration = e->dwDuration; /* FF_INFINITE maps to G923_INFINITE (same 0xffffffff) */
    fx->start_delay = e->dwStartDelay;
    fx->gain = (e->dwGain > 0) ? (int32_t)e->dwGain : G923_NOMINAL_MAX;
    fx->direction_deg = 0;
    if (e->cAxes >= 1 && e->rglDirection) {
        if (e->dwFlags & FFEFF_POLAR)      fx->direction_deg = (int32_t)(e->rglDirection[0] / 100);
        else if (e->dwFlags & FFEFF_CARTESIAN) fx->direction_deg = (e->rglDirection[0] < 0) ? 270 : 90;
    }
    if (e->lpEnvelope && e->lpEnvelope->dwSize >= sizeof(FFENVELOPE)) {
        fx->env.present = true;
        fx->env.attack_level = (int32_t)e->lpEnvelope->dwAttackLevel;
        fx->env.attack_time  = e->lpEnvelope->dwAttackTime;
        fx->env.fade_level   = (int32_t)e->lpEnvelope->dwFadeLevel;
        fx->env.fade_time    = e->lpEnvelope->dwFadeTime;
    } else {
        fx->env.present = false;
    }

    void *tsp = e->lpvTypeSpecificParams;
    switch (fx->kind) {
    case G923_FX_CONSTANT:
        if (tsp && e->cbTypeSpecificParams >= sizeof(FFCONSTANTFORCE))
            fx->constant_level = (int32_t)((FFCONSTANTFORCE *)tsp)->lMagnitude;
        break;
    case G923_FX_RAMP:
        if (tsp && e->cbTypeSpecificParams >= sizeof(FFRAMPFORCE)) {
            fx->ramp_start = (int32_t)((FFRAMPFORCE *)tsp)->lStart;
            fx->ramp_end   = (int32_t)((FFRAMPFORCE *)tsp)->lEnd;
        }
        break;
    case G923_FX_PERIODIC:
        fx->wave = wave_for_uuid(type);
        if (tsp && e->cbTypeSpecificParams >= sizeof(FFPERIODIC)) {
            FFPERIODIC *p = (FFPERIODIC *)tsp;
            fx->periodic_magnitude = (int32_t)p->dwMagnitude;
            fx->periodic_offset    = (int32_t)p->lOffset;
            fx->periodic_phase     = p->dwPhase;
            fx->periodic_period    = p->dwPeriod;
        }
        break;
    case G923_FX_SPRING:
    case G923_FX_DAMPER:
    case G923_FX_FRICTION:
    case G923_FX_INERTIA:
        if (tsp && e->cbTypeSpecificParams >= sizeof(FFCONDITION)) {
            FFCONDITION *c = (FFCONDITION *)tsp; /* first axis */
            fx->cond_center    = (int32_t)c->lOffset;
            fx->cond_pos_coeff = (int32_t)c->lPositiveCoefficient;
            fx->cond_neg_coeff = (int32_t)c->lNegativeCoefficient;
            fx->cond_pos_sat   = (int32_t)c->dwPositiveSaturation;
            fx->cond_neg_sat   = (int32_t)c->dwNegativeSaturation;
            fx->cond_deadband  = (int32_t)c->lDeadBand;
        }
        break;
    default: break;
    }
}

/* ---- IUnknown ----------------------------------------------------------- */
static HRESULT qi(G923Plugin *s, REFIID iid, LPVOID *ppv);
static ULONG addref(G923Plugin *s) { return ++s->refCount; }
static ULONG release(G923Plugin *s) {
    if (--s->refCount) return s->refCount;
    s->ticker_run = false;
    if (s->ticker) pthread_join(s->ticker, NULL);
    if (s->hid_ready) g923_hid_close(&s->hid);
    pthread_mutex_destroy(&s->lock);
    CFPlugInRemoveInstanceForFactory(s->factoryID);
    CFRelease(s->factoryID);
    free(s);
    return 0;
}
static HRESULT cfQI(void *self, REFIID iid, LPVOID *ppv) { return qi(fromCF(self), iid, ppv); }
static ULONG   cfAdd(void *self) { return addref(fromCF(self)); }
static ULONG   cfRel(void *self) { return release(fromCF(self)); }
static HRESULT ffQI(void *self, REFIID iid, LPVOID *ppv) { return qi(fromFF(self), iid, ppv); }
static ULONG   ffAdd(void *self) { return addref(fromFF(self)); }
static ULONG   ffRel(void *self) { return release(fromFF(self)); }

/* ---- IOCFPlugInInterface ----------------------------------------------- */
static IOReturn plugProbe(void *self, CFDictionaryRef p, io_service_t svc, SInt32 *order) {
    (void)self; (void)p; (void)svc;
    if (order) *order = 100000;
    return kIOReturnSuccess;
}
static IOReturn plugStart(void *self, CFDictionaryRef p, io_service_t svc) {
    (void)p;
    fromCF(self)->service = svc;
    return kIOReturnSuccess;
}
static IOReturn plugStop(void *self) { (void)self; return kIOReturnSuccess; }

/* ---- IOForceFeedbackDeviceInterface ------------------------------------ */
static HRESULT ffGetVersion(void *self, ForceFeedbackVersion *v) {
    (void)self;
    v->apiVersion.majorRev = kFFPlugInAPIMajorRev;
    v->apiVersion.minorAndBugRev = kFFPlugInAPIMinorAndBugRev;
    v->apiVersion.stage = kFFPlugInAPIStage;
    v->apiVersion.nonRelRev = kFFPlugInAPINonRelRev;
    v->plugInVersion.majorRev = 0;
    v->plugInVersion.minorAndBugRev = 1;
    v->plugInVersion.stage = developStage;
    v->plugInVersion.nonRelRev = 0;
    return FF_OK;
}

static HRESULT ffInitTerm(void *self, NumVersion api, io_object_t hid, boolean_t begin) {
    (void)api;
    G923Plugin *s = fromFF(self);
    if (begin) {
        pthread_mutex_lock(&s->lock);
        g923_engine_init(&s->engine);
        s->epoch_us = now_us();
        if (g923_hid_wrap(&s->hid, hid) && g923_hid_open(&s->hid)) {
            s->hid_ready = true;
            uint8_t cmd[G923_CMD_LEN];
            /* init sequence (mirrors new-lg4ff / G HUB): autocenter off,
             * default range, fixed-time-loop off, then zero every slot. */
            g923_cmd_autocenter_off(cmd);                g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_set_range(cmd, DEFAULT_RANGE_DEG);  g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_timeloop(cmd, false);               g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_constant(cmd, 0, 0);                g923_hid_send(&s->hid, 0, cmd);
            for (int slot = 1; slot < 4; slot++) { g923_cmd_stop(cmd, slot); g923_hid_send(&s->hid, 0, cmd); }
            LOGP("initialized: hid ready (max_out=%u), range=%d",
                 s->hid.max_output_len, DEFAULT_RANGE_DEG);
        } else {
            s->hid_ready = false;
            LOGP("initialized: WARNING hid not ready (open failed)");
        }
        if (!s->ticker) {
            s->ticker_run = true;
            pthread_create(&s->ticker, NULL, ticker_main, s);
        }
        pthread_mutex_unlock(&s->lock);
    } else {
        pthread_mutex_lock(&s->lock);
        g923_engine_reset(&s->engine);
        if (s->hid_ready) {
            uint8_t cmd[G923_CMD_LEN];
            g923_cmd_constant(cmd, 0, 0); g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_stop(cmd, 1); g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_stop(cmd, 2); g923_hid_send(&s->hid, 0, cmd);
            g923_cmd_stop(cmd, 3); g923_hid_send(&s->hid, 0, cmd);
        }
        pthread_mutex_unlock(&s->lock);
    }
    return FF_OK;
}

static HRESULT ffDownload(void *self, CFUUIDRef type, FFEffectDownloadID *pID,
                          FFEFFECT *e, FFEffectParameterFlag flags) {
    (void)flags;
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    int h = (int)*pID;
    if (h == 0) {
        h = g923_engine_alloc(&s->engine);
        if (h == 0) { pthread_mutex_unlock(&s->lock); return FFERR_DEVICEFULL; }
        *pID = (FFEffectDownloadID)h;
    }
    g923_effect *fx = g923_engine_get(&s->engine, h);
    if (!fx) { pthread_mutex_unlock(&s->lock); return FFERR_INVALIDDOWNLOADID; }
    bool was_playing = fx->playing;
    uint64_t st = fx->start_time_us;
    uint32_t it = fx->iterations;
    parse_effect(fx, type, e);
    fx->allocated = true;
    fx->playing = was_playing;
    fx->start_time_us = st;
    fx->iterations = it ? it : 1;
    if (s->hid_ready) send_slot_forces(s);
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffDestroy(void *self, FFEffectDownloadID id) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    g923_engine_free(&s->engine, (int)id);
    if (s->hid_ready) send_slot_forces(s);
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffStart(void *self, FFEffectDownloadID id, FFEffectStartFlag mode, UInt32 iters) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    if (mode & FFES_SOLO) g923_engine_stop_all(&s->engine);
    g923_engine_start(&s->engine, (int)id, iters, now_us());
    if (s->hid_ready) send_slot_forces(s);
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffStop(void *self, FFEffectDownloadID id) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    g923_engine_stop(&s->engine, (int)id);
    if (s->hid_ready) send_slot_forces(s);
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffGetStatus(void *self, FFEffectDownloadID id, FFEffectStatusFlag *st) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    g923_effect *fx = g923_engine_get(&s->engine, (int)id);
    *st = (fx && fx->playing) ? FFEGES_PLAYING : FFEGES_NOTPLAYING;
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffCaps(void *self, FFCAPABILITIES *c) {
    (void)self;
    memset(c, 0, sizeof(*c));
    c->ffSpecVer.majorRev = 1;
    c->supportedEffects = FFCAP_ET_CONSTANTFORCE | FFCAP_ET_RAMPFORCE |
        FFCAP_ET_SQUARE | FFCAP_ET_SINE | FFCAP_ET_TRIANGLE |
        FFCAP_ET_SAWTOOTHUP | FFCAP_ET_SAWTOOTHDOWN |
        FFCAP_ET_SPRING | FFCAP_ET_DAMPER | FFCAP_ET_INERTIA | FFCAP_ET_FRICTION;
    c->emulatedEffects = 0;
    c->subType = FFCAP_ST_KINESTHETIC;
    c->numFfAxes = 1;
    c->ffAxes[0] = FFJOFS_X;
    c->storageCapacity = 4;
    c->playbackCapacity = 4;
    return FF_OK;
}

static HRESULT ffState(void *self, ForceFeedbackDeviceState *ds) {
    G923Plugin *s = fromFF(self);
    ds->dwState = 0;
    pthread_mutex_lock(&s->lock);
    ds->dwState |= s->engine.paused ? FFGFFS_PAUSED : FFGFFS_STOPPED;
    ds->dwState |= s->engine.actuators_on ? FFGFFS_ACTUATORSON : FFGFFS_ACTUATORSOFF;
    ds->dwState |= FFGFFS_POWERON;
    pthread_mutex_unlock(&s->lock);
    ds->dwLoad = 0;
    return FF_OK;
}

static HRESULT ffCommand(void *self, FFCommandFlag cmd) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    switch (cmd) {
    case FFSFFC_RESET:          g923_engine_reset(&s->engine); break;
    case FFSFFC_STOPALL:        g923_engine_stop_all(&s->engine); break;
    case FFSFFC_PAUSE:          s->engine.paused = true; break;
    case FFSFFC_CONTINUE:       s->engine.paused = false; break;
    case FFSFFC_SETACTUATORSON: s->engine.actuators_on = true; break;
    case FFSFFC_SETACTUATORSOFF:s->engine.actuators_on = false; break;
    default: break;
    }
    if (s->hid_ready) send_slot_forces(s);
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffSetProp(void *self, FFProperty prop, void *val) {
    G923Plugin *s = fromFF(self);
    pthread_mutex_lock(&s->lock);
    if (prop == FFPROP_FFGAIN && val) {
        s->engine.device_gain = (int32_t)*(UInt32 *)val;
    } else if (prop == FFPROP_AUTOCENTER && val && s->hid_ready) {
        UInt32 on = *(UInt32 *)val;
        uint8_t a[G923_CMD_LEN], b[G923_CMD_LEN];
        if (on) { g923_cmd_autocenter_on(a, b, 0x8000); g923_hid_send(&s->hid,0,a); g923_hid_send(&s->hid,0,b); }
        else    { g923_cmd_autocenter_off(a); g923_hid_send(&s->hid,0,a); }
    }
    pthread_mutex_unlock(&s->lock);
    return FF_OK;
}

static HRESULT ffEscape(void *self, FFEffectDownloadID id, FFEFFESCAPE *esc) {
    (void)self; (void)id; (void)esc; return FFERR_UNSUPPORTED;
}

/* ---- vtables ------------------------------------------------------------ */
static IOCFPlugInInterface gCF = {
    NULL, cfQI, cfAdd, cfRel, 1, 0, plugProbe, plugStart, plugStop
};
static IOForceFeedbackDeviceInterface gFF = {
    NULL, ffQI, ffAdd, ffRel,
    ffGetVersion, ffInitTerm, ffDestroy, ffDownload, ffEscape, ffGetStatus,
    ffCaps, ffState, ffCommand, ffSetProp, ffStart, ffStop
};

static HRESULT qi(G923Plugin *s, REFIID iid, LPVOID *ppv) {
    CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE; *ppv = NULL;
    if (CFEqual(u, IUnknownUUID) || CFEqual(u, kIOCFPlugInInterfaceID)) { *ppv = &s->cfPlugInVtbl; r = S_OK; }
    else if (CFEqual(u, kIOForceFeedbackDeviceInterfaceID)) { *ppv = &s->ffVtbl; r = S_OK; }
    if (r == S_OK) addref(s);
    CFRelease(u);
    return r;
}

/* ---- factory ------------------------------------------------------------ */
void *G923ForceFeedbackFactory(CFAllocatorRef allocator, CFUUIDRef typeID);
void *G923ForceFeedbackFactory(CFAllocatorRef allocator, CFUUIDRef typeID) {
    (void)allocator;
    if (!CFEqual(typeID, kIOForceFeedbackLibTypeID)) return NULL;
    G923Plugin *s = (G923Plugin *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfPlugInVtbl = &gCF;
    s->ffVtbl = &gFF;
    s->factoryID = kG923FactoryID;
    CFRetain(s->factoryID);
    s->refCount = 1;
    pthread_mutex_init(&s->lock, NULL);
    CFPlugInAddInstanceForFactory(s->factoryID);
    return &s->cfPlugInVtbl;
}
