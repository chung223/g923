/* g923_find.h — locate Logitech wheel IOHIDDevice service nodes. */
#ifndef G923_FIND_H
#define G923_FIND_H

#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>

/* Returns true if (vid,pid) is a wheel this project handles. */
bool g923_is_supported_wheel(uint16_t vid, uint16_t pid);

/* Human-readable model name for a supported pid, or "Unknown". */
const char *g923_wheel_name(uint16_t pid);

/* Find the first supported wheel's IOHIDDevice service (caller releases with
 * IOObjectRelease). Returns IO_OBJECT_NULL if none. Fills vid/pid if non-NULL. */
io_service_t g923_find_wheel(uint16_t *vid_out, uint16_t *pid_out);

/* Find a specific HID interface of a supported wheel by primary usage page/usage
 * (e.g. 0xFFFD / 0xFD01 for the TrueForce vendor interface). Caller releases.
 * Returns IO_OBJECT_NULL if not present. */
io_service_t g923_find_wheel_iface(uint16_t usage_page, uint16_t usage,
                                   uint16_t *vid_out, uint16_t *pid_out);

#endif /* G923_FIND_H */
