/* g923_hid.c — see g923_hid.h */
#include "g923_hid.h"
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

static uint16_t get_u16(IOHIDDeviceRef d, CFStringRef key) {
    if (!d) return 0;
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    int32_t n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n);
    return (uint16_t)n;
}

bool g923_hid_wrap(g923_hid *h, io_service_t service) {
    h->dev = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    h->open = false;
    h->max_output_len = 0;
    if (h->dev) {
        h->max_output_len = get_u16(h->dev, CFSTR(kIOHIDMaxOutputReportSizeKey));
    }
    return h->dev != NULL;
}

bool g923_hid_open(g923_hid *h) {
    if (!h->dev) return false;
    if (h->open) return true;
    /* kIOHIDOptionsTypeNone = shared open; the game may also hold it open for
     * input. Output reports are still deliverable on a shared open. */
    IOReturn r = IOHIDDeviceOpen(h->dev, kIOHIDOptionsTypeNone);
    h->open = (r == kIOReturnSuccess);
    return h->open;
}

void g923_hid_close(g923_hid *h) {
    if (h->dev) {
        if (h->open) IOHIDDeviceClose(h->dev, kIOHIDOptionsTypeNone);
        CFRelease(h->dev);
    }
    h->dev = NULL;
    h->open = false;
}

bool g923_hid_send(g923_hid *h, uint8_t report_id, const uint8_t cmd[G923_CMD_LEN]) {
    if (!h->dev || !h->open) return false;
    /* The wheel's output report is longer than our 7-byte command (16 bytes on
     * the G923 c266). IOHIDDeviceSetReport must be given the full report length,
     * zero-padded, or the wheel ignores it. Fall back to 7 if the device did
     * not advertise a size. */
    uint8_t buf[64] = {0};
    uint32_t len = h->max_output_len;
    if (len < G923_CMD_LEN) len = G923_CMD_LEN;
    if (len > sizeof(buf)) len = sizeof(buf);
    memcpy(buf, cmd, G923_CMD_LEN);
    IOReturn r = IOHIDDeviceSetReport(h->dev, kIOHIDReportTypeOutput,
                                      (CFIndex)report_id, buf, (CFIndex)len);
    return r == kIOReturnSuccess;
}

uint16_t g923_hid_vendor(g923_hid *h)  { return get_u16(h->dev, CFSTR(kIOHIDVendorIDKey)); }
uint16_t g923_hid_product(g923_hid *h) { return get_u16(h->dev, CFSTR(kIOHIDProductIDKey)); }
