/*
 * g923_probe_if2 — TrueForce interface probe & descriptor dumper (v2).
 *
 * Run this WITH the wheel attached (native mode) to capture the facts the
 * TrueForce module needs but which are not publicly documented:
 *   - every HID interface the wheel exposes (usage page/usage, in/out report
 *     sizes), so we can positively identify IF0 (joystick), IF1 (HID++,
 *     0xFF43/HID++), and IF2 (TrueForce, 0xFFFD/0xFD01);
 *   - the raw HID report descriptor bytes of each interface;
 *   - the exact max output report size of the TrueForce interface.
 *
 *   g923_probe_if2                 dump all interfaces of the attached wheel
 *   g923_probe_if2 --dump-desc     also hex-dump each report descriptor
 *   g923_probe_if2 --silence N     (DANGER) send N silence frames to IF2 and
 *                                  report whether the wheel accepts them
 *
 * The --silence mode writes to the wheel; the byte format is UNVERIFIED, so it
 * only sends the "silence" value and is gated behind an explicit flag.
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g923_find.h"
#include "g923_trueforce.h"
#include "g923_protocol.h"

static long propn(io_service_t s, CFStringRef k) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    long n = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) { int32_t t=0; CFNumberGetValue((CFNumberRef)v,kCFNumberSInt32Type,&t); n=t; }
    if (v) CFRelease(v);
    return n;
}
static void props(io_service_t s, CFStringRef k, char *out, size_t n) {
    out[0]=0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v)==CFStringGetTypeID()) CFStringGetCString((CFStringRef)v,out,n,kCFStringEncodingUTF8);
    if (v) CFRelease(v);
}
static void dump_desc(io_service_t s) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR(kIOHIDReportDescriptorKey), kCFAllocatorDefault, 0);
    if (!v || CFGetTypeID(v) != CFDataGetTypeID()) { printf("      (no report descriptor)\n"); if(v)CFRelease(v); return; }
    CFDataRef d = (CFDataRef)v;
    const uint8_t *b = CFDataGetBytePtr(d);
    CFIndex len = CFDataGetLength(d);
    printf("      report descriptor (%ld bytes):\n", (long)len);
    for (CFIndex i = 0; i < len; i += 16) {
        printf("        ");
        for (CFIndex j = i; j < i+16 && j < len; j++) printf("%02x ", b[j]);
        printf("\n");
    }
    CFRelease(v);
}

static int dump_all(bool desc) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOHIDDevice"), &it) != KERN_SUCCESS)
        return 1;
    io_service_t s; int found = 0;
    while ((s = IOIteratorNext(it))) {
        long vid = propn(s, CFSTR(kIOHIDVendorIDKey));
        long pid = propn(s, CFSTR(kIOHIDProductIDKey));
        if (g923_is_supported_wheel((uint16_t)vid, (uint16_t)pid)) {
            found++;
            char prod[256]; props(s, CFSTR(kIOHIDProductKey), prod, sizeof prod);
            long up = propn(s, CFSTR(kIOHIDPrimaryUsagePageKey));
            long u  = propn(s, CFSTR(kIOHIDPrimaryUsageKey));
            long in = propn(s, CFSTR(kIOHIDMaxInputReportSizeKey));
            long out= propn(s, CFSTR(kIOHIDMaxOutputReportSizeKey));
            const char *role = "?";
            if (up == 0x01 && (u == 0x04 || u == 0x05)) role = "IF0 joystick/gamepad (classic FFB)";
            else if (up == 0xFF43 || up == 0xFF00)      role = "HID++ (do NOT drive)";
            else if (up == (long)G923_TF_USAGE_PAGE && u == (long)G923_TF_USAGE) role = "IF2 TrueForce (v2)";
            else                                        role = "vendor/other";
            printf("- %s  (0x%04lx:0x%04lx)\n", prod, vid, pid);
            printf("    usagePage=0x%04lx usage=0x%04lx  maxIn=%ld maxOut=%ld  => %s\n",
                   up, u, in, out, role);
            if (desc) dump_desc(s);
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!found) { printf("No supported Logitech wheel attached.\n"); return 1; }
    return 0;
}

static int silence_test(int frames) {
    g923_trueforce tf;
    if (!g923_tf_open(&tf)) return 1;
    printf("Sending %d silence frames to the TrueForce interface (format UNVERIFIED)...\n", frames);
    int ok = 0;
    for (int i = 0; i < frames; i++) if (g923_tf_silence(&tf)) ok++;
    printf("accepted %d/%d frames (SetReport success)\n", ok, frames);
    g923_tf_close(&tf);
    return ok > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    bool desc = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-desc")) desc = true;
        else if (!strcmp(argv[i], "--silence") && i+1 < argc) return silence_test(atoi(argv[++i]));
        else { fprintf(stderr, "usage: g923_probe_if2 [--dump-desc] [--silence N]\n"); return 2; }
    }
    printf("=== G923 HID interfaces (v2 TrueForce probe) ===\n");
    return dump_all(desc);
}
