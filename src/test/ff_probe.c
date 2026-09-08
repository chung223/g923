/*
 * ff_probe — a stand-in "game": drives force feedback through Apple's
 * ForceFeedback.framework against a named HID device. Used to exercise the real
 * G923FF.plugin end-to-end (framework -> our plugin) without a wheel, and to
 * check whether FF is wired up for a device.
 *
 * usage: ff_probe "<Product name>"
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <ForceFeedback/ForceFeedback.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static io_service_t find_by_product(const char *prod) {
    io_iterator_t it;
    IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOHIDDevice"), &it);
    io_service_t s, found = 0;
    CFStringRef want = CFStringCreateWithCString(NULL, prod, kCFStringEncodingUTF8);
    while ((s = IOIteratorNext(it))) {
        CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR(kIOHIDProductKey), NULL, 0);
        if (p && CFGetTypeID(p) == CFStringGetTypeID() && CFEqual(p, want)) { found = s; CFRelease(p); break; }
        if (p) CFRelease(p);
        IOObjectRelease(s);
    }
    IOObjectRelease(it); CFRelease(want);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ff_probe \"<Product name>\"\n"); return 2; }
    io_service_t s = find_by_product(argv[1]);
    if (!s) { fprintf(stderr, "device not found: %s\n", argv[1]); return 1; }

    HRESULT h = FFIsForceFeedback(s);
    printf("FFIsForceFeedback -> 0x%x (%s)\n", (unsigned)h, h == FF_OK ? "FF_OK" : "not FF");
    if (h != FF_OK) return 1;

    FFDeviceObjectReference dev = NULL;
    h = FFCreateDevice(s, &dev);
    printf("FFCreateDevice -> 0x%x\n", (unsigned)h);
    if (h != FF_OK) return 1;

    FFCAPABILITIES caps;
    if (FFDeviceGetForceFeedbackCapabilities(dev, &caps) == FF_OK)
        printf("caps: supported=0x%x axes=%u subType=%u\n",
               caps.supportedEffects, caps.numFfAxes, caps.subType);

    UInt32 gain = 8000; FFDeviceSetForceFeedbackProperty(dev, FFPROP_FFGAIN, &gain);
    UInt32 ac = 0;      FFDeviceSetForceFeedbackProperty(dev, FFPROP_AUTOCENTER, &ac);

    FFCONSTANTFORCE cf = { .lMagnitude = 8000 };
    DWORD axes[1] = { FFJOFS_X };
    LONG dir[1] = { 0 };
    FFEFFECT e; memset(&e, 0, sizeof e);
    e.dwSize = sizeof e;
    e.dwFlags = FFEFF_CARTESIAN | FFEFF_OBJECTOFFSETS;
    e.dwDuration = FF_INFINITE;
    e.dwGain = 10000;
    e.dwTriggerButton = FFJOFS_BUTTON(0) - FFJOFS_BUTTON(0) - 1; /* no trigger */
    e.cAxes = 1; e.rgdwAxes = axes; e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof cf; e.lpvTypeSpecificParams = &cf;

    FFEffectObjectReference eff = NULL;
    h = FFDeviceCreateEffect(dev, kFFEffectType_ConstantForce_ID, &e, &eff);
    printf("create constant effect -> 0x%x\n", (unsigned)h);
    if (h == FF_OK) {
        printf("playing constant force ~2s\n");
        FFEffectStart(eff, 1, 0);
        sleep(2);
        FFEffectStop(eff);
        FFDeviceReleaseEffect(dev, eff);
    }
    FFDeviceSendForceFeedbackCommand(dev, FFSFFC_RESET);
    FFReleaseDevice(dev);
    printf("OK\n");
    return 0;
}
