#!/bin/bash
# End-to-end test WITHOUT the wheel: prove that a game calling
# ForceFeedback.framework loads and drives our real G923FF.plugin.
#
# We stand in a virtual AppleUserHIDDevice for the wheel. The plugin's HID
# open/send will no-op on it (it isn't a real wheel), but the framework->plugin
# path, effect translation, and vtable are all exercised. The plugin logs to
# os_log under subsystem [G923FF]; we surface those lines.
set -uo pipefail
cd "$(dirname "$0")/.."
BUILD=build
PLUGIN="$PWD/$BUILD/G923FF.plugin"
PROBE="$BUILD/ff_probe"

echo "Plugin bundle:        $PLUGIN"
echo

# Build a tiny injector inline (same logic as g923ctl inject, but by Product name).
CINJ="$BUILD/_e2e_inject"
cat > "$BUILD/_e2e_inject.c" <<'EOF'
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdio.h>
#include <string.h>
#define FF "F4545CE5-BF5B-11D6-A4BB-0003933E3E3E"
static io_service_t byprod(const char*p){io_iterator_t it;IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOHIDDevice"),&it);io_service_t s,f=0;CFStringRef w=CFStringCreateWithCString(NULL,p,kCFStringEncodingUTF8);while((s=IOIteratorNext(it))){CFTypeRef q=IORegistryEntryCreateCFProperty(s,CFSTR(kIOHIDProductKey),NULL,0);if(q&&CFGetTypeID(q)==CFStringGetTypeID()&&CFEqual(q,w)){f=s;CFRelease(q);break;}if(q)CFRelease(q);IOObjectRelease(s);}IOObjectRelease(it);CFRelease(w);return f;}
int main(int c,char**v){io_service_t s=byprod(v[1]);if(!s){fprintf(stderr,"nodev\n");return 1;}
 CFTypeRef cur=IORegistryEntryCreateCFProperty(s,CFSTR("IOCFPlugInTypes"),NULL,0);
 CFMutableDictionaryRef m=cur&&CFGetTypeID(cur)==CFDictionaryGetTypeID()?CFDictionaryCreateMutableCopy(NULL,0,cur):CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 if(!strcmp(v[2],"--remove"))CFDictionaryRemoveValue(m,CFSTR(FF));else{CFStringRef p=CFStringCreateWithCString(NULL,v[2],kCFStringEncodingUTF8);CFDictionarySetValue(m,CFSTR(FF),p);}
 CFMutableDictionaryRef pr=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);CFDictionarySetValue(pr,CFSTR("IOCFPlugInTypes"),m);
 kern_return_t kr=IORegistryEntrySetCFProperties(s,pr);printf("inject->0x%x\n",kr);return kr?1:0;}
EOF
clang -o "$CINJ" "$BUILD/_e2e_inject.c" -framework IOKit -framework CoreFoundation || exit 1

# Compute the ../-escape relative value for the absolute plugin path.
REL="../../..${PLUGIN}"

# A real wheel's IOHIDDevice node accepts userspace property writes. Not every
# HID node does (Apple's SPU devices reject with 0xe00002c7). Pick the first
# AppleUserHIDDevice whose injection actually succeeds to stand in for the wheel.
echo "== selecting a stand-in device that accepts injection =="
# Candidates: all IOHIDDevice products, but try controller/virtual-like ones
# first (their nodes accept userspace property writes; Apple SPU nodes reject).
ALL="$(ioreg -c IOHIDDevice -r -l 2>/dev/null | awk -F'"' '/"Product" =/{print $4}' | sort -u)"
PREF="$(printf '%s\n' "$ALL" | grep -iE 'virtual|razer|wheel|joystic|controller|gamepad|g923|g29|logitech' || true)"
DEV=""
try_one() { "$CINJ" "$1" "$REL" >/dev/null 2>&1; }
for cand in $(printf '%s\n' "$PREF"); do [ -z "$cand" ] && continue; if try_one "$cand"; then DEV="$cand"; break; fi; done
if [ -z "$DEV" ]; then
  while IFS= read -r cand; do [ -z "$cand" ] && continue; if try_one "$cand"; then DEV="$cand"; break; fi; done <<< "$ALL"
fi
if [ -z "$DEV" ]; then
  echo "no injectable HID stand-in found (need a virtual HID device or a real wheel)"; exit 1
fi
echo "Using stand-in device: '$DEV'"
echo "== injected FF plugin (relative escape) =="
"$CINJ" "$DEV" "$REL"

echo; echo "== running ff_probe (stand-in game) =="
START=$(date "+%Y-%m-%d %H:%M:%S")
"./$PROBE" "$DEV"
RC=$?

echo; echo "== plugin os_log output =="
sleep 1
log show --start "$START" --predicate 'eventMessage CONTAINS "[G923FF]"' --info 2>/dev/null \
  | grep "\[G923FF\]" | sed 's/^/  /' | tail -20
echo "  (no lines above means the plugin logged nothing; check RC)"

echo; echo "== cleanup =="
"$CINJ" "$DEV" --remove >/dev/null
rm -f "$CINJ" "$BUILD/_e2e_inject.c"

echo
if [ $RC -eq 0 ]; then echo "E2E RESULT: PASS (framework loaded and drove G923FF.plugin)"; else echo "E2E RESULT: ff_probe rc=$RC"; fi
exit $RC
