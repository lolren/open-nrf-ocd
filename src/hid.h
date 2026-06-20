/* hid.h - platform-independent HID device interface.
 *
 * The platform-specific implementation (hid_linux.c, hid_macos.c,
 * hid_windows.c) exposes a thin synchronous API that all other layers
 * (probe.c, cmsis_dap.c) sit on top of. We deliberately avoid libusb /
 * hidapi so that nrf_ocd can be redistributed as a single self-contained
 * binary per OS.
 *
 * The data model:
 *   - hid_init() once at startup.
 *   - hid_enumerate() walks the OS USB tree looking for HID-class devices
 *     whose interface subclass / protocol matches CMSIS-DAP v1 (no
 *     descriptor) or v2 (DAP v2 in interface string).
 *   - hid_open_path() opens a specific device node / path.
 *   - hid_read() / hid_write() are blocking with a configurable timeout.
 *
 * CMSIS-DAP v1 reports have report ID 0 (so the first byte of every HID
 * report is the command id). v2 reports are 64 bytes with the command byte
 * at offset 0 and a 16-bit length prefix at offset 1..2 little-endian.
 * The HID report length is the negotiated INPUT/OUTPUT report length from
 * the OS; nrf_ocd assumes the standard 64-byte HID report unless the OS
 * tells us otherwise.
 */
#ifndef NRF_OCD_HID_H
#define NRF_OCD_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A CMSIS-DAP HID payload is 64 bytes. Windows includes the leading report
 * ID byte in HidP_*ReportByteLength, so keep room for 64 + report ID.
 */
#define NRF_OCD_HID_REPORT_SIZE 65

typedef struct hid_device hid_device_t;

/* Describes a probe before opening. */
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char     serial_number[64];
    char     product_string[128];
    char     manufacturer[128];
    char     path[512];   /* OS-specific path/identifier (e.g. /dev/hidraw3). */
    uint16_t usage_page;  /* HID usage page (0xFF00 = vendor). */
    uint16_t usage;       /* HID usage. CMSIS-DAP v1 = 0x0000 HID. */
    int      interface_number;
    bool     is_dap_v2;   /* True if HID reports look like v2. */
} hid_device_info_t;

typedef struct hid_enumerate_handle hid_enumerate_handle_t;

/* ----- Library lifecycle --------------------------------------------------- */
nrf_ocd_status_t hid_init(void);
void             hid_shutdown(void);

/* ----- Enumeration --------------------------------------------------------- */
hid_enumerate_handle_t *hid_enumerate_start(void);
const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h);
void             hid_enumerate_free(hid_enumerate_handle_t *h);

/* Find a probe by its USB serial number. Returns NULL if not present. */
const hid_device_info_t *hid_find_by_serial(const char *serial);
const hid_device_info_t *hid_find_by_index(unsigned index);

/* ----- Open / close -------------------------------------------------------- */
hid_device_t *hid_open_path(const char *path);
void          hid_close(hid_device_t *dev);

/* Non-blocking poll: returns true if a report is available. */
bool          hid_has_report(hid_device_t *dev);

/* Read one report into buf. Returns NRF_OCD_OK on success, NRF_OCD_ERR_TIMEOUT
 * on timeout, or another error code. The first byte of the report is the
 * CMSIS-DAP command id (for v1) or report id (for v2). */
nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms);

/* Write one report. data[0] is the report id / command id. */
nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms);

/* ----- Helpers ------------------------------------------------------------- */
const char *hid_path(const hid_device_t *dev);
const char *hid_serial(const hid_device_t *dev);
uint16_t    hid_vid(const hid_device_t *dev);
uint16_t    hid_pid(const hid_device_t *dev);
int         hid_report_size(const hid_device_t *dev);

/* Mark the device as a USB bulk transport (libusb backend). The CMSIS-DAP
 * layer uses this to decide whether to prepend a 0 byte on writes. */
void        hid_mark_bulk(hid_device_t *dev);
bool        hid_is_bulk(const hid_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_HID_H */
