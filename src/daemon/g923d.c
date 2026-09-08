/*
 * g923d — background agent that makes the wheel force-feedback-ready.
 *
 * On every appearance of a supported Logitech wheel it:
 *   1. injects the ForceFeedback plugin into the wheel's IOCFPlugInTypes so
 *      games' ForceFeedback.framework calls find it (the "../"-escape trick);
 *   2. (optionally) switches a PlayStation-mode G923 to native mode;
 *   3. sets a default rotation range.
 *
 * Runs as a launchd LaunchAgent. The bundle path is passed with --plugin and
 * must be an absolute path to the installed G923FF.plugin.
 *
 * Usage: g923d --plugin /path/to/G923FF.plugin [--range 900] [--no-mode-switch] [--once]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include "g923_find.h"
#include "g923_inject.h"
#include "g923_hid.h"
#include "g923_protocol.h"

static const char *g_plugin_path = NULL;
static uint16_t    g_range = 900;
static bool        g_mode_switch = true;
static bool        g_once = false;

static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "g923d: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static uint16_t prop_u16(io_service_t s, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    int32_t n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n);
    if (v) CFRelease(v);
    return (uint16_t)n;
}

static void handle_wheel(io_service_t svc, uint16_t vid, uint16_t pid) {
    log_msg("wheel appeared: %s (0x%04x:0x%04x)", g923_wheel_name(pid), vid, pid);

    bool ok = g923_inject_set(svc, g_plugin_path);
    char *rb = g923_inject_get(svc);
    log_msg("  FF plugin registered: %s (%s)", ok ? "OK" : "FAILED", rb ? rb : "none");
    free(rb);

    g923_hid h;
    if (g923_hid_wrap(&h, svc) && g923_hid_open(&h)) {
        if (g_mode_switch && pid == G923_PID_G923_PS) {
            uint8_t c[G923_CMD_LEN];
            g923_cmd_ps_to_native(c);
            g923_hid_send(&h, G923_REPORT_ID_PS_SWITCH, c);
            log_msg("  sent PS->native mode switch; wheel will re-enumerate as 0x%04x",
                    G923_PID_G923_NATIVE);
        } else {
            uint8_t c[G923_CMD_LEN];
            g923_cmd_set_range(c, g_range);          g923_hid_send(&h, 0, c);
            g923_cmd_autocenter_off(c);              g923_hid_send(&h, 0, c);
            log_msg("  set range=%u, autocenter off", g_range);
        }
        g923_hid_close(&h);
    } else {
        log_msg("  (could not open wheel for output; range/mode not set)");
    }
}

static void matched_cb(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        uint16_t vid = prop_u16(svc, CFSTR(kIOHIDVendorIDKey));
        uint16_t pid = prop_u16(svc, CFSTR(kIOHIDProductIDKey));
        if (g923_is_supported_wheel(vid, pid))
            handle_wheel(svc, vid, pid);
        IOObjectRelease(svc);
    }
}

static void run_once(void) {
    uint16_t vid, pid;
    io_service_t svc = g923_find_wheel(&vid, &pid);
    if (svc == IO_OBJECT_NULL) { log_msg("no wheel attached"); return; }
    handle_wheel(svc, vid, pid);
    IOObjectRelease(svc);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--plugin") && i + 1 < argc) g_plugin_path = argv[++i];
        else if (!strcmp(argv[i], "--range") && i + 1 < argc) g_range = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-mode-switch")) g_mode_switch = false;
        else if (!strcmp(argv[i], "--once")) g_once = true;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!g_plugin_path || g_plugin_path[0] != '/') {
        fprintf(stderr, "g923d: --plugin <absolute bundle path> is required\n");
        return 2;
    }

    if (g_once) { run_once(); return 0; }

    /* Watch for wheels appearing. IOServiceAddMatchingNotification's first
     * callback also delivers already-attached devices. */
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceAddMatchingNotification(
        port, kIOMatchedNotification, IOServiceMatching("IOHIDDevice"),
        matched_cb, NULL, &it);
    if (kr != KERN_SUCCESS) { log_msg("failed to register notification: 0x%x", kr); return 1; }
    matched_cb(NULL, it);   /* drain existing */
    log_msg("watching for Logitech wheels; plugin=%s", g_plugin_path);
    CFRunLoopRun();
    return 0;
}
