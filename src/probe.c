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

/* The CMSIS-DAP spec requires the product string to contain "CMSIS-DAP", so
 * accept such probes from vendors not listed above (e.g. Flipper Zero DAP
 * Link, which uses ST's 0x0483). */
static bool is_known_probe(const hid_device_info_t *info) {
    return is_known_vid(info->vendor_id) ||
           strstr(info->product_string, "CMSIS-DAP") != NULL;
}

static const hid_device_info_t *find_known_probe_by_index(unsigned index) {
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info;
    unsigned filtered_index = 0;
    static hid_device_info_t copy;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (!is_known_probe(info)) continue;
        if (filtered_index++ == index) {
            copy = *info;
            hid_enumerate_free(h);
            return &copy;
        }
    }
    hid_enumerate_free(h);
    return NULL;
}

static const hid_device_info_t *find_only_known_probe(void) {
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info;
    static hid_device_info_t copy;
    unsigned count = 0;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (!is_known_probe(info)) continue;
        copy = *info;
        count++;
        if (count > 1) break;
    }
    hid_enumerate_free(h);
    return count == 1 ? &copy : NULL;
}

nrf_ocd_status_t probe_list(probe_info_t *out, size_t max_count, size_t *out_count) {
    if (!out || !out_count) return NRF_OCD_ERR_INVALID_ARG;
    *out_count = 0;
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NRF_OCD_ERR_IO;
    const hid_device_info_t *info;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (!is_known_probe(info)) continue;
        if (*out_count >= max_count) break;
        probe_info_t *p = &out[*out_count];
        p->vendor_id  = info->vendor_id;
        p->product_id = info->product_id;
        nrf_ocd_str_copy(p->serial, sizeof(p->serial), info->serial_number);
        nrf_ocd_str_copy(p->product, sizeof(p->product), info->product_string);
        nrf_ocd_str_copy(p->manufacturer, sizeof(p->manufacturer), info->manufacturer);
        nrf_ocd_str_copy(p->path, sizeof(p->path), info->path);
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
        if (!info) {
            info = find_only_known_probe();
            if (info) {
                LOG_WARNING("Probe UID %s was not readable from USB descriptors; "
                            "using the only CMSIS-DAP probe at %s",
                            serial, info->path);
            }
        }
    } else {
        info = find_known_probe_by_index(index);
    }
    if (!info) return NRF_OCD_ERR_PROBE_NOT_FOUND;
    if (!is_known_probe(info)) {
        LOG_WARNING("Probe 0x%04x:0x%04x is not a known CMSIS-DAP vendor; "
                    "opening anyway.", info->vendor_id, info->product_id);
    }
    hid_device_t *dev = hid_open_path(info->path);
    if (!dev) return NRF_OCD_ERR_PROBE_OPEN;
    /* Prefer CMSIS-DAP v2 bulk transports when enumeration found one. Seeed
     * XIAO nRF54 probes expose HID too, but their HID path is not reliable
     * enough for flashing on all hosts. */
    bool want_bulk = info->is_dap_v2 ||
                     strncmp(info->path, "usb:", 4) == 0 ||
                     strncmp(info->path, "winusb:", 7) == 0;
    LOG_DEBUG("probe_open: vid=0x%04x pid=0x%04x path=%s want_bulk=%d",
              info->vendor_id, info->product_id, info->path, want_bulk ? 1 : 0);
    if (want_bulk) {
        hid_mark_bulk(dev);
    }
    if (out_info) {
        out_info->vendor_id  = info->vendor_id;
        out_info->product_id = info->product_id;
        nrf_ocd_str_copy(out_info->serial, sizeof(out_info->serial), info->serial_number);
        nrf_ocd_str_copy(out_info->product, sizeof(out_info->product), info->product_string);
        nrf_ocd_str_copy(out_info->manufacturer, sizeof(out_info->manufacturer),
                         info->manufacturer);
        nrf_ocd_str_copy(out_info->path, sizeof(out_info->path), info->path);
    }
    *out_dev = dev;
    return NRF_OCD_OK;
}
