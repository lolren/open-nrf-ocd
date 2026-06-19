/* usb_backend_macos.c - macOS HID backend for nrf_ocd
 *
 * Thin adapter: wraps the Open_nrf hid_macos.c IOKit HID functions
 * behind the old v1.0.4 nrf_probe_* API so the rest of the codebase
 * (cmsis_dap.c, coresight.c, nrf_ocd.c) works unchanged.
 */
#include "nrf_ocd.h"
#include "hid.h"          /* Open_nrf HID abstraction layer */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Global HID enumerator state. */
static hid_enumerate_handle_t *g_enum_h = NULL;
static int g_enum_idx = 0;

nrf_ocd_error_t nrf_probe_enum(nrf_probe_t **out_list, int *out_count) {
    if (!out_list || !out_count) return NRF_OCD_ERR_INVALID_ARG;
    
    /* Count probes first */
    int count = 0;
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) { *out_count = 0; *out_list = NULL; return NRF_OCD_OK; }
    
    while (hid_enumerate_next(h)) count++;
    hid_enumerate_free(h);
    
    if (count == 0) {
        *out_count = 0;
        *out_list = NULL;
        return NRF_OCD_OK;
    }
    
    /* Allocate probe list */
    nrf_probe_t *probes = (nrf_probe_t *)calloc((size_t)count, sizeof(nrf_probe_t));
    if (!probes) return NRF_OCD_ERR_NO_MEM;
    
    /* Fill probe list */
    h = hid_enumerate_start();
    if (!h) { free(probes); *out_count = 0; *out_list = NULL; return NRF_OCD_OK; }
    
    int idx = 0;
    const hid_device_info_t *info;
    while ((info = hid_enumerate_next(h)) != NULL && idx < count) {
        probes[idx].vid = info->vendor_id;
        probes[idx].pid = info->product_id;
        snprintf(probes[idx].serial, sizeof(probes[idx].serial), "%s",
                 info->serial ? info->serial : "");
        snprintf(probes[idx].product, sizeof(probes[idx].product), "%s",
                 info->product_string ? info->product_string : "");
        snprintf(probes[idx].path, sizeof(probes[idx].path), "%s",
                 info->path ? info->path : "");
        idx++;
    }
    hid_enumerate_free(h);
    
    *out_count = count;
    *out_list = probes;
    return NRF_OCD_OK;
}

void nrf_probe_free_list(nrf_probe_t *list, int count) {
    (void)count;
    free(list);
}

nrf_ocd_error_t nrf_probe_open(nrf_probe_t *probe) {
    if (!probe) return NRF_OCD_ERR_INVALID_ARG;
    hid_device_t *dev = hid_open_path(probe->path);
    if (!dev) return NRF_OCD_ERR_PROBE_OPEN;
    probe->handle = (void *)dev;
    /* Check if this is a v2 (bulk) device */
    if (hid_is_bulk(dev)) {
        probe->flags |= 1; /* bulk flag */
    }
    return NRF_OCD_OK;
}

void nrf_probe_close(nrf_probe_t *probe) {
    if (probe && probe->handle) {
        hid_close((hid_device_t *)probe->handle);
        probe->handle = NULL;
    }
}

nrf_ocd_error_t nrf_probe_write(nrf_probe_t *probe, const uint8_t *data, int len) {
    if (!probe || !probe->handle || !data || len <= 0)
        return NRF_OCD_ERR_INVALID_ARG;
    return hid_write((hid_device_t *)probe->handle, data, (size_t)len, 5000);
}

nrf_ocd_error_t nrf_probe_read(nrf_probe_t *probe, uint8_t *buf, int buf_size, int *out_len) {
    if (!probe || !probe->handle || !buf || buf_size <= 0 || !out_len)
        return NRF_OCD_ERR_INVALID_ARG;
    size_t actual = 0;
    nrf_ocd_status_t st = hid_read((hid_device_t *)probe->handle, buf,
                                    (size_t)buf_size, 5000, &actual);
    *out_len = (int)actual;
    return st;
}

int nrf_probe_get_report_size(nrf_probe_t *probe) {
    if (!probe || !probe->handle) return 64;
    hid_device_t *dev = (hid_device_t *)probe->handle;
    return hid_report_size(dev) > 0 ? hid_report_size(dev) : 64;
}
