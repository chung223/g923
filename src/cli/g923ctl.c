/*
 * g923ctl — command-line control & diagnostics for the G923 Mac driver.
 *
 *   g923ctl list                       list attached Logitech wheels + FF status
 *   g923ctl inject <abs-bundle-path>   register the FF plugin on the wheel node
 *   g923ctl clear                      remove our FF plugin registration
 *   g923ctl status                     show whether FF is wired up
 *   g923ctl range <degrees>            set rotation range (e.g. 900)
 *   g923ctl led <0-31>                 set the rev-counter LED bitmask
 *   g923ctl autocenter <on|off>        firmware auto-center spring
 *   g923ctl mode-native                switch a PS-mode G923 to native mode
 *   g923ctl force <-32768..32767>      send a raw constant force (hold ~2s)
 *   g923ctl stop                       zero all forces
 *
 * Commands that talk to the wheel need it attached; without it they report and
 * exit non-zero rather than doing anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "g923_find.h"
#include "g923_inject.h"
#include "g923_hid.h"
#include "g923_protocol.h"

static int need_wheel(io_service_t *svc, uint16_t *vid, uint16_t *pid) {
    *svc = g923_find_wheel(vid, pid);
    if (*svc == IO_OBJECT_NULL) {
        fprintf(stderr, "no supported Logitech wheel found (is it plugged in?)\n");
        return -1;
    }
    return 0;
}

static int cmd_list(void) {
    uint16_t vid, pid;
    io_service_t svc = g923_find_wheel(&vid, &pid);
    if (svc == IO_OBJECT_NULL) { printf("No supported Logitech wheel attached.\n"); return 1; }
    uint64_t entryID = 0; IORegistryEntryGetRegistryEntryID(svc, &entryID);
    char *ff = g923_inject_get(svc);
    printf("Found: %s  (VID 0x%04x PID 0x%04x, registryID 0x%llx)\n",
           g923_wheel_name(pid), vid, pid, (unsigned long long)entryID);
    printf("  Force-feedback plugin: %s\n", ff ? ff : "(not registered)");
    if (pid == G923_PID_G923_PS)
        printf("  Note: wheel is in PlayStation mode; run 'g923ctl mode-native' for full FFB.\n");
    free(ff);
    IOObjectRelease(svc);
    return 0;
}

static int cmd_inject(const char *path) {
    if (!path || path[0] != '/') { fprintf(stderr, "need an absolute bundle path\n"); return 2; }
    io_service_t svc; uint16_t vid, pid;
    if (need_wheel(&svc, &vid, &pid)) return 1;
    char *rel = g923_inject_make_relative(path);
    printf("Injecting FF plugin:\n  bundle:   %s\n  registry: %s\n", path, rel ? rel : "?");
    free(rel);
    bool ok = g923_inject_set(svc, path);
    char *rb = g923_inject_get(svc);
    printf("  result:   %s\n  readback: %s\n", ok ? "OK" : "FAILED", rb ? rb : "(none)");
    free(rb);
    IOObjectRelease(svc);
    return ok ? 0 : 1;
}

static int cmd_clear(void) {
    io_service_t svc; uint16_t vid, pid;
    if (need_wheel(&svc, &vid, &pid)) return 1;
    bool ok = g923_inject_clear(svc);
    printf("Cleared FF plugin registration: %s\n", ok ? "OK" : "FAILED");
    IOObjectRelease(svc);
    return ok ? 0 : 1;
}

static int with_open_wheel(int (*fn)(g923_hid *, void *), void *ctx) {
    io_service_t svc; uint16_t vid, pid;
    if (need_wheel(&svc, &vid, &pid)) return 1;
    g923_hid h;
    if (!g923_hid_wrap(&h, svc) || !g923_hid_open(&h)) {
        fprintf(stderr, "could not open the wheel for output\n");
        IOObjectRelease(svc); return 1;
    }
    int rc = fn(&h, ctx);
    g923_hid_close(&h);
    IOObjectRelease(svc);
    return rc;
}

static int do_range(g923_hid *h, void *ctx) {
    uint16_t deg = *(uint16_t *)ctx;
    uint8_t c[G923_CMD_LEN]; g923_cmd_set_range(c, deg);
    bool ok = g923_hid_send(h, 0, c);
    printf("set range %u deg: %s\n", deg, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
static int do_led(g923_hid *h, void *ctx) {
    uint8_t mask = *(uint8_t *)ctx;
    uint8_t c[G923_CMD_LEN]; g923_cmd_set_leds(c, mask);
    bool ok = g923_hid_send(h, 0, c);
    printf("set LEDs 0x%02x: %s\n", mask, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
static int do_autocenter(g923_hid *h, void *ctx) {
    int on = *(int *)ctx;
    uint8_t a[G923_CMD_LEN], b[G923_CMD_LEN];
    bool ok;
    if (on) { g923_cmd_autocenter_on(a, b, 0x8000); ok = g923_hid_send(h,0,a) && g923_hid_send(h,0,b); }
    else    { g923_cmd_autocenter_off(a); ok = g923_hid_send(h,0,a); }
    printf("autocenter %s: %s\n", on ? "on" : "off", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
static int do_mode_native(g923_hid *h, void *ctx) {
    (void)ctx;
    uint8_t c[G923_CMD_LEN];
    g923_cmd_ps_to_native(c);
    bool ok = g923_hid_send(h, G923_REPORT_ID_PS_SWITCH, c);
    printf("PS->native switch (report id 0x30): %s\n", ok ? "sent" : "FAILED");
    printf("The wheel should re-enumerate as PID 0x%04x; re-run 'list'.\n", G923_PID_G923_NATIVE);
    return ok ? 0 : 1;
}
static int do_force(g923_hid *h, void *ctx) {
    int level = *(int *)ctx;
    uint8_t c[G923_CMD_LEN]; g923_cmd_constant(c, 0, level);
    printf("constant force %d for 2s (byte=0x%02x)...\n", level, g923_translate_force(level));
    bool ok = g923_hid_send(h, 0, c);
    sleep(2);
    g923_cmd_constant(c, 0, 0); g923_hid_send(h, 0, c);
    printf("done: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
static int do_stop(g923_hid *h, void *ctx) {
    (void)ctx; uint8_t c[G923_CMD_LEN];
    for (int slot = 0; slot < 4; slot++) { g923_cmd_stop(c, slot); g923_hid_send(h, 0, c); }
    g923_cmd_constant(c, 0, 0); g923_hid_send(h, 0, c);
    printf("all forces stopped\n");
    return 0;
}

static void usage(void) {
    fprintf(stderr,
      "usage: g923ctl <command>\n"
      "  list | status\n"
      "  inject <abs-bundle-path> | clear\n"
      "  range <degrees> | led <0-31> | autocenter <on|off>\n"
      "  mode-native | force <-32768..32767> | stop\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list") || !strcmp(cmd, "status")) return cmd_list();
    if (!strcmp(cmd, "inject")) return argc >= 3 ? cmd_inject(argv[2]) : (usage(), 2);
    if (!strcmp(cmd, "clear"))  return cmd_clear();
    if (!strcmp(cmd, "range")) {
        if (argc < 3) { usage(); return 2; }
        uint16_t d = (uint16_t)atoi(argv[2]); return with_open_wheel(do_range, &d);
    }
    if (!strcmp(cmd, "led")) {
        if (argc < 3) { usage(); return 2; }
        uint8_t m = (uint8_t)(atoi(argv[2]) & 0x1f); return with_open_wheel(do_led, &m);
    }
    if (!strcmp(cmd, "autocenter")) {
        if (argc < 3) { usage(); return 2; }
        int on = !strcmp(argv[2], "on"); return with_open_wheel(do_autocenter, &on);
    }
    if (!strcmp(cmd, "mode-native")) return with_open_wheel(do_mode_native, NULL);
    if (!strcmp(cmd, "force")) {
        if (argc < 3) { usage(); return 2; }
        int l = atoi(argv[2]); return with_open_wheel(do_force, &l);
    }
    if (!strcmp(cmd, "stop")) return with_open_wheel(do_stop, NULL);
    usage();
    return 2;
}
