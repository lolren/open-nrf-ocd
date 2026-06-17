/* probe.c - USB / HID probe enumeration.
 *
 * Wraps hid.h with the small amount of policy that nrf_ocd needs:
 *   - Recognise CMSIS-DAP probes by VID (well-known set: 0x2886 Seeed,
 *     0x0D28 NXP, 0x1B6F Arm, 0xC251 Keil, 0x2E8A Raspberry Pi, etc.).
 *   - Return a probe list / select by serial or index.
 *   - Open the probe and return a hid_device_t ready for CMSIS-DAP use.
 */
#include "hid.h"
#include "log.h"
#include "nrf_ocd.h"
#include "probe.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Well-known CMSIS-DAP vendor IDs we recognise. pyOCD recognises a much
 * bigger set; we focus on what the user is likely to plug in. */
static const uint16_t known_vids[] = {
    0x2886, /* Seeed Studio (XIAO nRF54L15 = 0066, nRF54LM20A = 0068). */
    0x0D28, /* NXP / mbed. */
    0x1B6F, /* Arm / DAPLink. */
    0xC251, /* Keil. */
    0x2E8A, /* Raspberry Pi. */
    0x1209, /* pid.codes (open DAPLink-class). */
    0x16D0, /* MBS. */
    0x03EB, /* Atmel/Microchip. */
};

static bool is_known_vid(uint16_t vid) {
    for (size_t i = 0; i < sizeof(known_vids) / sizeof(known_vids[0]); i++) {
        if (known_vids[i] == vid) return true;
    }
    return false;
}

nrf_ocd_status_t probe_list(probe_info_t *out, size_t max_count, size_t *out_count) {
    if (!out || !out_count) return NRF_OCD_ERR_INVALID_ARG;
    *out_count = 0;
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NRF_OCD_ERR_IO;
    const hid_device_info_t *info;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (!is_known_vid(info->vendor_id)) continue;
        if (*out_count >= max_count) break;
        probe_info_t *p = &out[*out_count];
        p->vendor_id  = info->vendor_id;
        p->product_id = info->product_id;
        strncpy(p->serial,    info->serial_number, sizeof(p->serial)    - 1);
        strncpy(p->product,   info->product_string, sizeof(p->product)   - 1);
        strncpy(p->manufacturer, info->manufacturer, sizeof(p->manufacturer) - 1);
        strncpy(p->path,      info->path, sizeof(p->path) - 1);
        LOG_INFO("Probe %zu: %s [%s] at %s", *out_count, p->product, p->serial, p->path);
        (*out_count)++;
    }
    hid_enumerate_free(h);
    return NRF_OCD_OK;
}

nrf_ocd_status_t probe_open(probe_info_t *out_info, hid_device_t **out_dev,
                            const char *serial, unsigned index) {
    if (!out_dev) return NRF_OCD_ERR_INVALID_ARG;
    *out_dev = NULL;
    const hid_device_info_t *info = NULL;
    if (serial && serial[0]) {
        info = hid_find_by_serial(serial);
    } else {
        info = hid_find_by_index(index);
    }
    if (!info) return NRF_OCD_ERR_PROBE_NOT_FOUND;
    if (!is_known_vid(info->vendor_id)) {
        LOG_WARNING("Probe 0x%04x:0x%04x is not a known CMSIS-DAP vendor; "
                    "opening anyway.", info->vendor_id, info->product_id);
    }
    hid_device_t *dev = hid_open_path(info->path);
    if (!dev) return NRF_OCD_ERR_PROBE_OPEN;
    /* Seeed XIAO nRF54 boards (VID=0x2886) have broken HID DAP_Transfer
     * responses.  The working C implementation detects this by vendor ID
     * and switches to the v2 bulk backend automatically. */
    /* Seeed XIAO nRF54 boards (VID=0x2886) have broken HID DAP_Transfer
     * responses, but the bulk endpoint also has issues with writes through
     * the HID backend. The best approach is to only enable bulk when the
     * libusb backend is actually in use (path starts with "usb:"). */
    bool want_bulk = (info->vendor_id == 0x2886) && (strncmp(info->path, "usb:", 4) == 0);
    LOG_DEBUG("probe_open: vid=0x%04x pid=0x%04x path=%s want_bulk=%d",
              info->vendor_id, info->product_id, info->path, want_bulk ? 1 : 0);
    if (want_bulk) {
        extern void hid_mark_bulk(hid_device_t *dev);
        hid_mark_bulk(dev);
    }
    if (out_info) {
        out_info->vendor_id  = info->vendor_id;
        out_info->product_id = info->product_id;
        strncpy(out_info->serial,    info->serial_number, sizeof(out_info->serial)    - 1);
        strncpy(out_info->product,   info->product_string, sizeof(out_info->product)   - 1);
        strncpy(out_info->manufacturer, info->manufacturer, sizeof(out_info->manufacturer) - 1);
        strncpy(out_info->path,      info->path, sizeof(out_info->path) - 1);
    }
    *out_dev = dev;
    return NRF_OCD_OK;
}
