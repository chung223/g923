/* g923_inject.c — see g923_inject.h */
#include "g923_inject.h"
#include "g923_protocol.h"
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>
#include <stdlib.h>

/* The framework prepends this exact prefix (3 path components deep). */
#define SLE_PREFIX "/System/Library/Extensions/"
#define SLE_DEPTH  3   /* System, Library, Extensions */

char *g923_inject_make_relative(const char *abs_bundle_path) {
    if (!abs_bundle_path || abs_bundle_path[0] != '/') return NULL;
    /* Build "../" * SLE_DEPTH + (abs path without leading '/'). */
    size_t esc = SLE_DEPTH * 3;             /* "../" each */
    size_t tail = strlen(abs_bundle_path) - 1;
    char *out = (char *)malloc(esc + tail + 1);
    if (!out) return NULL;
    size_t p = 0;
    for (int i = 0; i < SLE_DEPTH; i++) { memcpy(out + p, "../", 3); p += 3; }
    memcpy(out + p, abs_bundle_path + 1, tail);
    out[p + tail] = '\0';
    return out;
}

static CFMutableDictionaryRef copy_plugin_types(io_service_t service) {
    CFTypeRef cur = IORegistryEntryCreateCFProperty(service, CFSTR("IOCFPlugInTypes"),
                                                    kCFAllocatorDefault, 0);
    CFMutableDictionaryRef m;
    if (cur && CFGetTypeID(cur) == CFDictionaryGetTypeID()) {
        m = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)cur);
    } else {
        m = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    }
    if (cur) CFRelease(cur);
    return m;
}

static bool set_plugin_types(io_service_t service, CFMutableDictionaryRef m) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR("IOCFPlugInTypes"), m);
    IOReturn r = IORegistryEntrySetCFProperties(service, props);
    CFRelease(props);
    return r == kIOReturnSuccess;
}

bool g923_inject_set(io_service_t service, const char *abs_bundle_path) {
    char *rel = g923_inject_make_relative(abs_bundle_path);
    if (!rel) return false;
    CFMutableDictionaryRef m = copy_plugin_types(service);
    CFStringRef key = CFSTR(G923_FF_PLUGIN_TYPE_UUID);
    CFStringRef val = CFStringCreateWithCString(NULL, rel, kCFStringEncodingUTF8);
    CFDictionarySetValue(m, key, val);
    bool ok = set_plugin_types(service, m);
    CFRelease(val);
    CFRelease(m);
    free(rel);
    return ok;
}

bool g923_inject_clear(io_service_t service) {
    CFMutableDictionaryRef m = copy_plugin_types(service);
    CFDictionaryRemoveValue(m, CFSTR(G923_FF_PLUGIN_TYPE_UUID));
    bool ok = set_plugin_types(service, m);
    CFRelease(m);
    return ok;
}

char *g923_inject_get(io_service_t service) {
    CFTypeRef cur = IORegistryEntryCreateCFProperty(service, CFSTR("IOCFPlugInTypes"),
                                                    kCFAllocatorDefault, 0);
    char *out = NULL;
    if (cur && CFGetTypeID(cur) == CFDictionaryGetTypeID()) {
        CFStringRef v = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)cur,
                                                          CFSTR(G923_FF_PLUGIN_TYPE_UUID));
        if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
            CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(v),
                                                          kCFStringEncodingUTF8) + 1;
            out = (char *)malloc(n);
            if (out && !CFStringGetCString(v, out, n, kCFStringEncodingUTF8)) {
                free(out); out = NULL;
            }
        }
    }
    if (cur) CFRelease(cur);
    return out;
}
