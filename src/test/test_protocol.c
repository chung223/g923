/*
 * test_protocol.c — hardware-free unit tests for the wheel protocol encoders
 * and the effects engine. Values marked "independent" are hand-derived from the
 * new-lg4ff reference; others check internal consistency and invariants.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "g923_protocol.h"
#include "g923_effects.h"

static int g_fail = 0, g_pass = 0;

static void hex(const uint8_t *b, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 3, "%02x ", b[i]);
}
static void check_bytes(const char *name, const uint8_t *got, const uint8_t *exp, int n) {
    if (memcmp(got, exp, n) == 0) { g_pass++; printf("  PASS %s\n", name); }
    else {
        g_fail++;
        char g[64] = {0}, e[64] = {0}; hex(got, n, g); hex(exp, n, e);
        printf("  FAIL %s\n       got: %s\n       exp: %s\n", name, g, e);
    }
}
static void check_int(const char *name, long got, long exp) {
    if (got == exp) { g_pass++; printf("  PASS %s (=%ld)\n", name, got); }
    else { g_fail++; printf("  FAIL %s: got %ld exp %ld\n", name, got, exp); }
}
static void check_true(const char *name, int cond) {
    if (cond) { g_pass++; printf("  PASS %s\n", name); }
    else { g_fail++; printf("  FAIL %s\n", name); }
}

static void test_encoders(void) {
    uint8_t c[G923_CMD_LEN], a[G923_CMD_LEN], b[G923_CMD_LEN];
    printf("[encoders]\n");

    /* constant force, independent expected values */
    g923_cmd_constant(c, 0, 0x7fff);
    check_bytes("constant slot0 max", c, (uint8_t[]){0x11,0x00,0xff,0,0,0,0}, 7);
    g923_cmd_constant(c, 0, 0);
    check_bytes("constant slot0 zero", c, (uint8_t[]){0x11,0x00,0x80,0,0,0,0}, 7);
    g923_cmd_constant(c, 0, -32768);
    check_bytes("constant slot0 min", c, (uint8_t[]){0x11,0x00,0x00,0,0,0,0}, 7);
    g923_cmd_constant(c, 1, 0x7fff);
    check_bytes("constant slot1 max (force at idx3)", c, (uint8_t[]){0x21,0x00,0x00,0xff,0,0,0}, 7);

    /* range 900 = 0x0384 LE */
    g923_cmd_set_range(c, 900);
    check_bytes("range 900", c, (uint8_t[]){0xf8,0x81,0x84,0x03,0,0,0}, 7);
    g923_cmd_set_range(c, 540);
    check_bytes("range 540", c, (uint8_t[]){0xf8,0x81,0x1c,0x02,0,0,0}, 7);

    /* LEDs */
    g923_cmd_set_leds(c, 0x1f);
    check_bytes("leds all", c, (uint8_t[]){0xf8,0x12,0x1f,0,0,0,0}, 7);

    /* autocenter off */
    g923_cmd_autocenter_off(c);
    check_bytes("autocenter off", c, (uint8_t[]){0xf5,0,0,0,0,0,0}, 7);

    /* autocenter on, mag 0x8000 -> derived expand values */
    g923_cmd_autocenter_on(a, b, 0x8000);
    check_bytes("autocenter set", a, (uint8_t[]){0xfe,0x0d,0x04,0x04,0x60,0,0}, 7);
    check_bytes("autocenter activate", b, (uint8_t[]){0x14,0,0,0,0,0,0}, 7);

    /* PS -> native */
    g923_cmd_ps_to_native(c);
    check_bytes("ps->native", c, (uint8_t[]){0xf8,0x09,0x07,0x01,0x01,0x00,0x00}, 7);

    /* condition tags + slot nibbles */
    g923_cmd_spring(c, 1, 5000, -5000, 0, 0, 10000);
    check_int("spring slot1 byte0", c[0], (0x10<<1)+G923_OP_DOWNLOAD);
    check_int("spring tag", c[1], 0x0b);
    g923_cmd_damper(c, 2, 5000, 5000, 10000);
    check_int("damper slot2 byte0", c[0], (0x10<<2)+G923_OP_DOWNLOAD);
    check_int("damper tag", c[1], 0x0c);
    g923_cmd_friction(c, 3, 5000, 5000, 10000);
    check_int("friction slot3 byte0", c[0], (0x10<<3)+G923_OP_DOWNLOAD);
    check_int("friction tag", c[1], 0x0e);
    g923_cmd_stop(c, 0);
    check_int("stop slot0 byte0", c[0], (0x10<<0)+G923_OP_STOP);
}

static void test_scaling(void) {
    printf("[scaling]\n");
    check_int("translate_force(0)", g923_translate_force(0), 0x80);
    check_int("translate_force(+max)", g923_translate_force(0x7fff), 0xff);
    check_int("translate_force(-max)", g923_translate_force(-32768), 0x00);
    check_int("translate_force clamp hi", g923_translate_force(100000), 0xff);
    check_int("translate_force clamp lo", g923_translate_force(-100000), 0x00);
    check_int("scale_u16(0xffff,8)", g923_scale_u16(0xffff, 8), 0xff);
    check_int("scale_coeff(0x4000,4)", g923_scale_coeff(0x4000, 4), (0x8000>>12));
}

static void test_engine_constant(void) {
    printf("[engine: constant force]\n");
    g923_engine e; g923_engine_init(&e);
    int h = g923_engine_alloc(&e);
    check_true("alloc handle", h >= 1);
    g923_effect *fx = g923_engine_get(&e, h);
    fx->kind = G923_FX_CONSTANT;
    fx->duration = G923_INFINITE;
    fx->constant_level = 10000;      /* full positive */
    fx->direction_deg = 90;          /* east -> + */
    g923_engine_start(&e, h, 1, 1000);

    g923_slot_forces f;
    g923_engine_tick(&e, 2000, &f);
    check_true("constant active", f.has_constant);
    check_int("constant full = +0x7fff", f.constant_level, 0x7fff);

    /* device gain 50% halves it */
    e.device_gain = 5000;
    g923_engine_tick(&e, 2000, &f);
    check_int("constant at 50% gain", f.constant_level, 0x7fff / 2);

    /* west direction flips sign */
    e.device_gain = 10000; fx->direction_deg = 270;
    g923_engine_tick(&e, 2000, &f);
    check_int("constant west = -0x7fff", f.constant_level, -0x7fff);

    /* stop -> no constant */
    g923_engine_stop(&e, h);
    g923_engine_tick(&e, 2000, &f);
    check_true("stopped -> no constant", !f.has_constant);

    /* encode: exactly one command, slot0, force byte present */
    fx->direction_deg = 90; g923_engine_start(&e, h, 1, 1000);
    g923_engine_tick(&e, 2000, &f);
    uint8_t cmds[4*G923_CMD_LEN];
    int n = g923_encode_slots(&f, cmds);
    check_int("encode count", n, 1);
    check_int("encoded slot0 op", cmds[0], (0x10<<0)+G923_OP_DOWNLOAD);
    check_int("encoded force byte", cmds[2], 0xff);
}

static void test_engine_duration(void) {
    printf("[engine: duration + delay]\n");
    g923_engine e; g923_engine_init(&e);
    int h = g923_engine_alloc(&e);
    g923_effect *fx = g923_engine_get(&e, h);
    fx->kind = G923_FX_CONSTANT; fx->duration = 1000 /*us*/; fx->constant_level = 10000; fx->direction_deg = 90;
    g923_engine_start(&e, h, 1, 0);
    g923_slot_forces f;
    g923_engine_tick(&e, 500, &f);   check_true("within duration active", f.has_constant);
    g923_engine_tick(&e, 1500, &f);  check_true("after duration inactive", !f.has_constant);

    /* start delay */
    g923_effect *fx2 = g923_engine_get(&e, h);
    fx2->start_delay = 1000; fx2->duration = 1000;
    g923_engine_start(&e, h, 1, 0);
    g923_engine_tick(&e, 500, &f);   check_true("during delay inactive", !f.has_constant);
    g923_engine_tick(&e, 1500, &f);  check_true("after delay active", f.has_constant);
}

static void test_engine_conditions(void) {
    printf("[engine: spring condition]\n");
    g923_engine e; g923_engine_init(&e);
    int h = g923_engine_alloc(&e);
    g923_effect *fx = g923_engine_get(&e, h);
    fx->kind = G923_FX_SPRING; fx->duration = G923_INFINITE;
    fx->cond_pos_coeff = 10000; fx->cond_neg_coeff = 10000;
    fx->cond_pos_sat = 10000; fx->cond_deadband = 0; fx->cond_center = 0;
    g923_engine_start(&e, h, 1, 0);
    g923_slot_forces f; g923_engine_tick(&e, 10, &f);
    check_true("spring active", f.has_spring);
    uint8_t cmds[4*G923_CMD_LEN];
    int n = g923_encode_slots(&f, cmds);
    check_int("spring encode count", n, 1);
    check_int("spring encoded tag", cmds[1], 0x0b);
    check_int("spring encoded slot1", cmds[0], (0x10<<1)+G923_OP_DOWNLOAD);
}

int main(void) {
    printf("=== G923 protocol / effects unit tests ===\n");
    test_encoders();
    test_scaling();
    test_engine_constant();
    test_engine_duration();
    test_engine_conditions();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
