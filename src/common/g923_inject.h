/* g923_inject.h — register the ForceFeedback plugin on a wheel's IOKit node.
 *
 * ForceFeedback.framework's FFIsForceFeedback gate builds the plugin path as
 * "/System/Library/Extensions/" + <IOCFPlugInTypes value> and stat()s the
 * executable there. It does not honor a leading '/'. We therefore inject a
 * RELATIVE value that escapes /System/Library/Extensions/ with enough "../"
 * to reach the real (user-writable) bundle path. Verified working on
 * macOS 26.2 / Apple Silicon with SIP enabled, as a normal user. */
#ifndef G923_INJECT_H
#define G923_INJECT_H

#include <IOKit/IOKitLib.h>
#include <stdbool.h>

/* Turn an absolute bundle path into the escaping relative value the framework
 * needs. Caller frees the returned string. E.g. "/opt/g923/G923FF.plugin" ->
 * "../../../opt/g923/G923FF.plugin". Returns NULL on bad input. */
char *g923_inject_make_relative(const char *abs_bundle_path);

/* Set IOCFPlugInTypes[FF_UUID] = relative-escape-of(abs_bundle_path) on the
 * given IOHIDDevice service, preserving existing plugin entries. */
bool g923_inject_set(io_service_t service, const char *abs_bundle_path);

/* Remove our IOCFPlugInTypes[FF_UUID] entry (leaves Apple's entries intact). */
bool g923_inject_clear(io_service_t service);

/* Read back the current FF plugin value for a service, or NULL if none.
 * Caller frees. */
char *g923_inject_get(io_service_t service);

#endif /* G923_INJECT_H */
