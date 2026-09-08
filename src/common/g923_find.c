/* g923_find.c — see g923_find.h */
#include "g923_find.h"
#include "g923_protocol.h"
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

bool g923_is_supported_wheel(uint16_t vid, uint16_t pid) {
    if (vid != G923_VENDOR_ID_LOGITECH) return false;
    switch (pid) {
    case G923_PID_G923_NATIVE:
    case G923_PID_G923_PS:
    case G923_PID_G923_XBOX:
    case G923_PID_G29:
    case G923_PID_DF_COMPAT:
    case G923_PID_G920:
        return true;
    default:
        return false;
    }
}

const char *g923_wheel_name(uint16_t pid) {
    switch (pid) {
    case G923_PID_G923_NATIVE: return "G923 (native/PC mode)";
    case G923_PID_G923_PS:     return "G923 (PlayStation mode)";
    case G923_PID_G923_XBOX:   return "G923 (Xbox mode)";
    case G923_PID_G29:         return "G29";
    case G923_PID_DF_COMPAT:   return "Driving Force (compat mode)";
    case G923_PID_G920:        return "G920";
    default:                   return "Unknown";
    }
}

static uint16_t prop_u16(io_service_t s, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    int32_t n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n);
    if (v) CFRelease(v);
    return (uint16_t)n;
}

/* The wheel exposes several HID interfaces: interface 0 is the joystick
 * (usage page 0x01 / usage 0x04) and is the only one that takes classic FFB
 * output reports. Interface 1 is HID++ and interface 2 is the TrueForce vendor
 * stream — we must NOT drive those. Prefer a node whose primary usage is
 * Generic Desktop / Joystick and that advertises an output report. */
static bool is_joystick_iface(io_service_t s) {
    uint16_t up = prop_u16(s, CFSTR(kIOHIDPrimaryUsagePageKey));
    uint16_t u  = prop_u16(s, CFSTR(kIOHIDPrimaryUsageKey));
    return up == 0x01 && (u == 0x04 /* joystick */ || u == 0x05 /* gamepad */);
}
static bool has_output(io_service_t s) {
    return prop_u16(s, CFSTR(kIOHIDMaxOutputReportSizeKey)) > 0;
}

io_service_t g923_find_wheel(uint16_t *vid_out, uint16_t *pid_out) {
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOHIDDevice"), &it) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    io_service_t s;
    io_service_t best = IO_OBJECT_NULL;     /* joystick iface with output */
    uint16_t best_vid = 0, best_pid = 0;
    io_service_t fallback = IO_OBJECT_NULL;  /* any matching node */
    uint16_t fb_vid = 0, fb_pid = 0;
    while ((s = IOIteratorNext(it))) {
        uint16_t vid = prop_u16(s, CFSTR(kIOHIDVendorIDKey));
        uint16_t pid = prop_u16(s, CFSTR(kIOHIDProductIDKey));
        if (!g923_is_supported_wheel(vid, pid)) { IOObjectRelease(s); continue; }
        if (is_joystick_iface(s) && has_output(s) && best == IO_OBJECT_NULL) {
            best = s; best_vid = vid; best_pid = pid;  /* keep */
        } else if (fallback == IO_OBJECT_NULL) {
            fallback = s; fb_vid = vid; fb_pid = pid;  /* keep */
        } else {
            IOObjectRelease(s);
        }
    }
    IOObjectRelease(it);
    io_service_t chosen = best;
    uint16_t v = best_vid, p = best_pid;
    if (chosen == IO_OBJECT_NULL) { chosen = fallback; v = fb_vid; p = fb_pid; }
    else if (fallback != IO_OBJECT_NULL) IOObjectRelease(fallback);
    if (chosen != IO_OBJECT_NULL) { if (vid_out) *vid_out = v; if (pid_out) *pid_out = p; }
    return chosen;
}
