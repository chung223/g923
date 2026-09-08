/* g923_hid.h — thin IOHIDDevice transport for sending 7-byte wheel commands. */
#ifndef G923_HID_H
#define G923_HID_H

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <stdbool.h>
#include "g923_protocol.h"

typedef struct {
    IOHIDDeviceRef dev;
    bool open;
    uint32_t max_output_len;   /* kIOHIDMaxOutputReportSize; reports zero-padded to this */
} g923_hid;

/* Wrap an io_service_t (an IOHIDDevice service node). Does not open it. */
bool g923_hid_wrap(g923_hid *h, io_service_t service);

/* Open for output. Returns true on success. */
bool g923_hid_open(g923_hid *h);
void g923_hid_close(g923_hid *h);

/* Send one 7-byte classic command as an output report. report_id is normally 0;
 * use 0x30 for the PS->native switch. Zero-pads to the device's max output
 * report size. Returns true on success. */
bool g923_hid_send(g923_hid *h, uint8_t report_id, const uint8_t cmd[G923_CMD_LEN]);

/* Send an arbitrary output report of exactly len bytes (used for the 64-byte
 * TrueForce stream on interface 2). Returns true on success. */
bool g923_hid_send_report(g923_hid *h, uint8_t report_id, const uint8_t *buf, size_t len);

/* Read VID/PID from the wrapped device. */
uint16_t g923_hid_vendor(g923_hid *h);
uint16_t g923_hid_product(g923_hid *h);

#endif /* G923_HID_H */
