/*
 * g923_protocol.h — Logitech G923 (and G29-family) wheel protocol constants and
 * force-feedback command encoders.
 *
 * All byte layouts are transcribed from the out-of-tree Linux driver
 * berarma/new-lg4ff (GPL-2.0), which is the reference implementation for the
 * G923 PlayStation-mode wheel. Mainline hid-lg4ff.c does NOT support 0xC267.
 *
 * These are the *classic* Logitech HID output-report commands. They do NOT
 * cover TrueForce (a separate proprietary high-frequency channel only reachable
 * through Logitech's Windows Steering Wheel SDK / HID++).
 *
 * A "command" is 7 data bytes. On the wire it is a HID output report; on the
 * G923 in native mode the report has no report-ID prefix (report id 0), except
 * the PS->native mode switch which must be sent with report id 0x30.
 */
#ifndef G923_PROTOCOL_H
#define G923_PROTOCOL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ---- USB identifiers ---------------------------------------------------- */
#define G923_VENDOR_ID_LOGITECH        0x046dU

#define G923_PID_G923_NATIVE           0xC266U /* "G923" / PC native mode      */
#define G923_PID_G923_PS               0xC267U /* G923 PlayStation mode        */
#define G923_PID_G923_XBOX             0xC26EU /* G923 Xbox (HID++, different)  */
#define G923_PID_G29                   0xC24FU /* G29 native (same FFB family)  */
#define G923_PID_DF_COMPAT             0xC294U /* Driving Force compat mode     */
#define G923_PID_G920                  0xC262U

/* HID report id used only for the PS->native switch. */
#define G923_REPORT_ID_PS_SWITCH       0x30

/* A classic command is always 7 data bytes. */
#define G923_CMD_LEN                   7

/* ---- scaling helpers (verbatim semantics from new-lg4ff) ---------------- */
static inline uint16_t g923_clamp_u16(int32_t x) {
    return (uint16_t)(x > 0xffff ? 0xffff : (x < 0 ? 0 : x));
}
static inline int16_t g923_clamp_s16(int32_t x) {
    if (x <= -0x8000) return (int16_t)-0x8000;
    if (x >  0x7fff)  return (int16_t) 0x7fff;
    return (int16_t)x;
}
/* SCALE_VALUE_U16(x,bits) = clamp_u16(x) >> (16-bits) */
static inline uint32_t g923_scale_u16(int32_t x, int bits) {
    return (uint32_t)g923_clamp_u16(x) >> (16 - bits);
}
/* SCALE_COEFF(x,bits) = SCALE_VALUE_U16(|x|*2, bits) */
static inline uint32_t g923_scale_coeff(int32_t x, int bits) {
    int32_t a = x < 0 ? -x : x;
    return g923_scale_u16(a * 2, bits);
}
/* TRANSLATE_FORCE(x) = (clamp_s16(x) + 0x8000) >> 8 ; 0x80 == zero force */
static inline uint8_t g923_translate_force(int32_t level) {
    return (uint8_t)(((int32_t)g923_clamp_s16(level) + 0x8000) >> 8);
}

/* ---- effect slot model -------------------------------------------------- */
/* The wheel firmware has 4 mixing slots (0..3). Slot command byte0 selects the
 * slot bitmask in the high nibble (0x10<<id) and the op in the low nibble. */
typedef enum {
    G923_OP_DOWNLOAD = 0x1,  /* create/refresh with new params */
    G923_OP_REFRESH  = 0xC,  /* re-issue existing params        */
    G923_OP_STOP     = 0x3,  /* stop the slot                   */
} g923_slot_op;

/* ---- command encoders --------------------------------------------------- */
/* Each writes exactly G923_CMD_LEN bytes into out[]. */

/* Constant force on a given slot. level is signed 16-bit (DirectInput scale
 * -0x8000..0x7fff). NOTE: like lg4ff, the force byte lands at index (2+slot). */
static inline void g923_cmd_constant(uint8_t out[G923_CMD_LEN], int slot, int32_t level) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = (uint8_t)((0x10 << slot) + G923_OP_DOWNLOAD);
    out[1] = 0x00;
    out[2 + slot] = g923_translate_force(level);
}

/* Spring condition. k1/k2 are signed coefficients (DirectInput scale), d1/d2 are
 * the deadband edges as signed positions, clip is the saturation (0..0xffff). */
static inline void g923_cmd_spring(uint8_t out[G923_CMD_LEN], int slot,
                                   int32_t k1, int32_t k2,
                                   int32_t d1_pos, int32_t d2_pos, int32_t clip) {
    uint32_t d1 = g923_scale_u16(((d1_pos) + 0x8000) & 0xffff, 11);
    uint32_t d2 = g923_scale_u16(((d2_pos) + 0x8000) & 0xffff, 11);
    int s1 = k1 < 0, s2 = k2 < 0;
    int32_t ak1 = k1 < 0 ? -k1 : k1;
    int32_t ak2 = k2 < 0 ? -k2 : k2;
    if (ak1 < 2048) d1 = 0;     else ak1 -= 2048;
    if (ak2 < 2048) d2 = 2047;  else ak2 -= 2048;
    out[0] = (uint8_t)((0x10 << slot) + G923_OP_DOWNLOAD);
    out[1] = 0x0b;
    out[2] = (uint8_t)(d1 >> 3);
    out[3] = (uint8_t)(d2 >> 3);
    out[4] = (uint8_t)((g923_scale_coeff(ak2, 4) << 4) + g923_scale_coeff(ak1, 4));
    out[5] = (uint8_t)(((d2 & 7) << 5) + ((d1 & 7) << 1) + (s2 << 4) + s1);
    out[6] = (uint8_t)g923_scale_u16(clip, 8);
}

/* Damper condition. */
static inline void g923_cmd_damper(uint8_t out[G923_CMD_LEN], int slot,
                                   int32_t k1, int32_t k2, int32_t clip) {
    int s1 = k1 < 0, s2 = k2 < 0;
    out[0] = (uint8_t)((0x10 << slot) + G923_OP_DOWNLOAD);
    out[1] = 0x0c;
    out[2] = (uint8_t)g923_scale_coeff(k1, 4);
    out[3] = (uint8_t)s1;
    out[4] = (uint8_t)g923_scale_coeff(k2, 4);
    out[5] = (uint8_t)s2;
    out[6] = (uint8_t)g923_scale_u16(clip, 8);
}

/* Friction condition. */
static inline void g923_cmd_friction(uint8_t out[G923_CMD_LEN], int slot,
                                     int32_t k1, int32_t k2, int32_t clip) {
    int s1 = k1 < 0, s2 = k2 < 0;
    out[0] = (uint8_t)((0x10 << slot) + G923_OP_DOWNLOAD);
    out[1] = 0x0e;
    out[2] = (uint8_t)g923_scale_coeff(k1, 8);
    out[3] = (uint8_t)g923_scale_coeff(k2, 8);
    out[4] = (uint8_t)g923_scale_u16(clip, 8);
    out[5] = (uint8_t)((s2 << 4) + s1);
    out[6] = 0x00;
}

/* Stop a slot. */
static inline void g923_cmd_stop(uint8_t out[G923_CMD_LEN], int slot) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = (uint8_t)((0x10 << slot) + G923_OP_STOP);
}

/* Set the wheel's physical rotation range in degrees (e.g. 900). */
static inline void g923_cmd_set_range(uint8_t out[G923_CMD_LEN], uint16_t degrees) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = 0xf8;
    out[1] = 0x81;
    out[2] = (uint8_t)(degrees & 0x00ff);
    out[3] = (uint8_t)((degrees & 0xff00) >> 8);
}

/* Rev-counter / mode LEDs. bitmask bit0..bit4 light the 5 LEDs progressively. */
static inline void g923_cmd_set_leds(uint8_t out[G923_CMD_LEN], uint8_t bitmask) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = 0xf8;
    out[1] = 0x12;
    out[2] = bitmask;
}

/* Disable the firmware auto-center spring. */
static inline void g923_cmd_autocenter_off(uint8_t out[G923_CMD_LEN]) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = 0xf5;
}

/* Program + activate the firmware auto-center spring.
 * magnitude 0..0xffff. Writes TWO commands: out_set then out_activate. */
static inline void g923_cmd_autocenter_on(uint8_t out_set[G923_CMD_LEN],
                                          uint8_t out_activate[G923_CMD_LEN],
                                          uint16_t magnitude) {
    uint32_t expand_a, expand_b;
    if (magnitude <= 0xaaaa) {
        expand_a = 0x0cU * magnitude;
        expand_b = 0x80U * magnitude;
    } else {
        expand_a = (0x0cU * 0xaaaaU) + 0x06U * (magnitude - 0xaaaaU);
        expand_b = (0x80U * 0xaaaaU) + 0xffU * (magnitude - 0xaaaaU);
    }
    /* Non-MOMO wheels (G923 included) halve expand_a. */
    expand_a >>= 1;
    memset(out_set, 0, G923_CMD_LEN);
    out_set[0] = 0xfe;
    out_set[1] = 0x0d;
    out_set[2] = (uint8_t)(expand_a / 0xaaaa);
    out_set[3] = (uint8_t)(expand_a / 0xaaaa);
    out_set[4] = (uint8_t)(expand_b / 0xaaaa);
    memset(out_activate, 0, G923_CMD_LEN);
    out_activate[0] = 0x14;
}

/* Fixed-time-loop control (0x0D). off = firmware does not auto-repeat effects. */
static inline void g923_cmd_timeloop(uint8_t out[G923_CMD_LEN], bool on) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = 0x0d;
    out[1] = on ? 0x01 : 0x00;
}

/* Stop every slot at once (0xF3). */
static inline void g923_cmd_stop_all(uint8_t out[G923_CMD_LEN]) {
    memset(out, 0, G923_CMD_LEN);
    out[0] = 0xf3;
}

/* PS-mode -> native G923 switch. Send this ONE command with report id 0x30. */
static inline void g923_cmd_ps_to_native(uint8_t out[G923_CMD_LEN]) {
    static const uint8_t c[G923_CMD_LEN] = {0xf8, 0x09, 0x07, 0x01, 0x01, 0x00, 0x00};
    memcpy(out, c, G923_CMD_LEN);
}

/* Generic/DF -> native G923 (ext09): send out_a then out_b, report id 0. */
static inline void g923_cmd_ext09_to_g923(uint8_t out_a[G923_CMD_LEN],
                                          uint8_t out_b[G923_CMD_LEN]) {
    static const uint8_t a[G923_CMD_LEN] = {0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t b[G923_CMD_LEN] = {0xf8, 0x09, 0x07, 0x01, 0x01, 0x00, 0x00};
    memcpy(out_a, a, G923_CMD_LEN);
    memcpy(out_b, b, G923_CMD_LEN);
}

/* The ForceFeedback plugin type UUID string, used as the IOCFPlugInTypes key. */
#define G923_FF_PLUGIN_TYPE_UUID "F4545CE5-BF5B-11D6-A4BB-0003933E3E3E"

#endif /* G923_PROTOCOL_H */
