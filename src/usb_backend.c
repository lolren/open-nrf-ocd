/*
 * usb_backend.c - USB transport for CMSIS-DAP v1 and v2
 *
 * v1: hidapi (HID interrupt endpoints)
 * v2: libusb (bulk endpoints)
 * Seeed XIAO nRF54: libusb bulk on v2 interface (64-byte packets)
 */

#include "nrf_ocd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hidapi.h>
#include <libusb-1.0/libusb.h>
#include <errno.h>
#include <wchar.h>

typedef struct { uint16_t vid; uint16_t pid; } known_vid_pid_t;

static const known_vid_pid_t known_cmsis_dap[] = {
    { 0x0d28, 0x0204 }, { 0x0d28, 0x0207 }, { 0x0d28, 0x0211 },
    { 0x0d28, 0x0213 }, { 0x0d28, 0x0214 }, { 0x0d28, 0x0217 },
    { 0x1366, 0x0101 }, { 0x1366, 0x0105 },
    { 0x2886, 0x0066 }, { 0x2886, 0x0068 },
    { 0x0483, 0x374b }, { 0x0483, 0x3748 }, { 0x0483, 0x374d },
    { 0x0483, 0x374e }, { 0x0483, 0x374c }, { 0x0483, 0x374a },
    { 0x2e8a, 0x000c }, /* Raspberry Pi Debugprobe on Pico (CMSIS-DAP v2) */
    { 0x0000, 0x0000 },
};

static bool is_known_cmsis_dap(uint16_t vid, uint16_t pid) {
    for (int i = 0; known_cmsis_dap[i].vid != 0; i++)
        if (known_cmsis_dap[i].vid == vid && known_cmsis_dap[i].pid == pid)
            return true;
    return false;
}

static void copy_str(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void wchar_to_utf8(const wchar_t *src, char *dst, size_t dst_size) {
    if (dst_size == 0)
        return;
    if (!src || !*src) { dst[0] = '\0'; return; }
    size_t wlen = wcslen(src);
    wchar_t *tmp = malloc((wlen + 1) * sizeof(wchar_t));
    if (!tmp) { dst[0] = '\0'; return; }
    wcsncpy(tmp, src, wlen + 1); tmp[wlen] = 0;
    const wchar_t *sp = tmp;
    mbstate_t st = {0};
    size_t r = wcsrtombs(dst, &sp, dst_size - 1, &st);
    if (r == (size_t)-1 || r >= dst_size) r = dst_size - 1;
    dst[r] = '\0'; free(tmp);
}

typedef struct {
    libusb_device_handle *handle;
    uint8_t ep_in;
    uint8_t ep_out;
    int packet_size;
    int iface_num;
} v2_ctx_t;

static libusb_device_handle *open_libusb_device(uint16_t vid, uint16_t pid, const char *serial) {
    libusb_device_handle *h = NULL;
    libusb_device **devs = NULL;
    ssize_t cnt = libusb_get_device_list(NULL, &devs);
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(dev, &dd) < 0) continue;
        if (dd.idVendor != vid || dd.idProduct != pid) continue;
        if (serial && serial[0]) {
            char s[256];
            libusb_device_handle *tmp;
            if (libusb_open(dev, &tmp) == 0) {
                if (libusb_get_string_descriptor_ascii(tmp, dd.iSerialNumber, (unsigned char *)s, sizeof(s)) > 0) {
                    if (strcmp(s, serial) == 0) { h = tmp; break; }
                }
                libusb_close(tmp);
            }
        } else {
            libusb_device_handle *tmp;
            if (libusb_open(dev, &tmp) == 0) { h = tmp; break; }
        }
    }
    libusb_free_device_list(devs, 1);
    return h;
}

static nrf_ocd_error_t ensure_libusb_init(void) {
    static int libusb_inited = 0;

    if (!libusb_inited) {
        int r = libusb_init(NULL);
        if (r < 0)
            return NRF_OCD_ERR_USB_OPEN;
        libusb_inited = 1;
    }

    return NRF_OCD_OK;
}

static nrf_ocd_error_t open_libusb_bulk_probe(nrf_probe_t *probe, int packet_size) {
    nrf_ocd_error_t err = ensure_libusb_init();
    if (err != NRF_OCD_OK)
        return err;

    const char *serial = strchr(probe->serial, ':') ? NULL : probe->serial;
    libusb_device_handle *h = open_libusb_device(probe->vid, probe->pid, serial);
    if (!h)
        return NRF_OCD_ERR_USB_OPEN;

    libusb_device *dev = libusb_get_device(h);
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
        libusb_close(h);
        return NRF_OCD_ERR_USB_OPEN;
    }

    uint8_t iface_num = 0xFF, ep_out = 0, ep_in = 0;
    for (int ci = 0; ci < cfg->bNumInterfaces; ci++) {
        for (int ai = 0; ai < cfg->interface[ci].num_altsetting; ai++) {
            const struct libusb_interface_descriptor *idesc = &cfg->interface[ci].altsetting[ai];
            if (idesc->bInterfaceClass != 0xFF || idesc->bInterfaceSubClass != 0)
                continue;

            uint8_t candidate_out = 0;
            uint8_t candidate_in = 0;
            for (int ei = 0; ei < idesc->bNumEndpoints; ei++) {
                const struct libusb_endpoint_descriptor *ep = &idesc->endpoint[ei];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if ((ep->bEndpointAddress & 0x80) == 0)
                    candidate_out = ep->bEndpointAddress;
                else
                    candidate_in = ep->bEndpointAddress;
            }

            if (candidate_out && candidate_in) {
                iface_num = idesc->bInterfaceNumber;
                ep_out = candidate_out;
                ep_in = candidate_in;
                break;
            }
        }
        if (ep_out && ep_in)
            break;
    }
    libusb_free_config_descriptor(cfg);

    if (iface_num == 0xFF || !ep_out || !ep_in) {
        libusb_close(h);
        return NRF_OCD_ERR_USB_OPEN;
    }

    if (libusb_kernel_driver_active(h, iface_num) == 1)
        libusb_detach_kernel_driver(h, iface_num);

    if (libusb_claim_interface(h, iface_num) < 0) {
        libusb_close(h);
        return NRF_OCD_ERR_USB_OPEN;
    }

    v2_ctx_t *ctx = calloc(1, sizeof(v2_ctx_t));
    if (!ctx) {
        libusb_release_interface(h, iface_num);
        libusb_close(h);
        return NRF_OCD_ERR_MEMORY;
    }

    ctx->handle = h;
    ctx->ep_in = ep_in;
    ctx->ep_out = ep_out;
    ctx->packet_size = packet_size;
    ctx->iface_num = iface_num;
    probe->ep_out = ep_out;
    probe->ep_in = ep_in;
    probe->hid_handle = ctx;
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_probe_enum(nrf_probe_t **out_list, int *out_count) {
    struct hid_device_info *devs, *cur;
    nrf_probe_t *list = calloc(8, sizeof(nrf_probe_t));
    if (!list) return NRF_OCD_ERR_MEMORY;
    int count = 0, capacity = 8;

    /* Phase 1: enumerate HID CMSIS-DAP probes (v1 and dual v1/v2) */
    devs = hid_enumerate(0x0000, 0x0000);
    cur = devs;
    while (cur) {
        bool is_cmsis_dap = false;
        if (is_known_cmsis_dap(cur->vendor_id, cur->product_id)) {
            is_cmsis_dap = true;
        } else {
            char p[256]; wchar_to_utf8(cur->product_string, p, sizeof(p));
            if (strstr(p, "CMSIS-DAP")) is_cmsis_dap = true;
        }
        if (!is_cmsis_dap) { cur = cur->next; continue; }
        if (count >= capacity) {
            capacity *= 2;
            nrf_probe_t *tmp = realloc(list, capacity * sizeof(nrf_probe_t));
            if (!tmp) { free(list); hid_free_enumeration(devs); return NRF_OCD_ERR_MEMORY; }
            list = tmp;
        }
        nrf_probe_t *probe = &list[count];
        memset(probe, 0, sizeof(*probe));
        probe->vid = cur->vendor_id; probe->pid = cur->product_id;
        probe->report_in_size = 64; probe->report_out_size = 64;
        copy_str(probe->path, sizeof(probe->path), cur->path);
        wchar_to_utf8(cur->serial_number, probe->serial, sizeof(probe->serial));
        if (probe->serial[0] == '\0') {
            snprintf(probe->serial, sizeof(probe->serial), "%04X:%04X:%08X",
                     cur->vendor_id, cur->product_id, (unsigned)count);
        }
        wchar_to_utf8(cur->manufacturer_string, probe->vendor, sizeof(probe->vendor));
        if (probe->vendor[0] == '\0') snprintf(probe->vendor, sizeof(probe->vendor), "%04X", cur->vendor_id);
        wchar_to_utf8(cur->product_string, probe->product, sizeof(probe->product));
        if (probe->product[0] == '\0') snprintf(probe->product, sizeof(probe->product), "%04X", cur->product_id);
        count++; cur = cur->next;
    }
    hid_free_enumeration(devs);

    /* Phase 2: scan libusb for CMSIS-DAP v2 (bulk-only) probes that have
     * no HID interface and thus are missed by hid_enumerate. */
    ensure_libusb_init();
    libusb_device **ldevs = NULL;
    ssize_t lcnt = libusb_get_device_list(NULL, &ldevs);
    for (ssize_t i = 0; i < lcnt; i++) {
        libusb_device *dev = ldevs[i];
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(dev, &dd) < 0) continue;

        /* Skip if already found by HID enumeration */
        bool already_listed = false;
        for (int j = 0; j < count; j++) {
            if (list[j].vid == dd.idVendor && list[j].pid == dd.idProduct) {
                already_listed = true;
                break;
            }
        }
        if (already_listed) continue;

        /* Check for CMSIS-DAP v2 interface (class 0xFF, subclass 0) */
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) < 0) continue;

        bool has_dap_v2 = false;
        for (int ci = 0; ci < cfg->bNumInterfaces && !has_dap_v2; ci++) {
            for (int ai = 0; ai < cfg->interface[ci].num_altsetting; ai++) {
                const struct libusb_interface_descriptor *idesc =
                    &cfg->interface[ci].altsetting[ai];
                if (idesc->bInterfaceClass == 0xFF &&
                    idesc->bInterfaceSubClass == 0) {
                    /* Check interface string for "CMSIS-DAP" */
                    libusb_device_handle *tmp;
                    if (libusb_open(dev, &tmp) == 0) {
                        char ifstr[128] = {0};
                        libusb_get_string_descriptor_ascii(tmp,
                            idesc->iInterface, (unsigned char *)ifstr, sizeof(ifstr));
                        if (strstr(ifstr, "CMSIS-DAP")) {
                            has_dap_v2 = true;
                        }
                        libusb_close(tmp);
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);

        if (!has_dap_v2) continue;

        /* Read product/serial strings */
        libusb_device_handle *tmp;
        if (libusb_open(dev, &tmp) != 0) continue;

        char product[128] = {0}, serial[128] = {0}, vendor[128] = {0};
        libusb_get_string_descriptor_ascii(tmp, dd.iProduct,
            (unsigned char *)product, sizeof(product));
        libusb_get_string_descriptor_ascii(tmp, dd.iSerialNumber,
            (unsigned char *)serial, sizeof(serial));
        libusb_get_string_descriptor_ascii(tmp, dd.iManufacturer,
            (unsigned char *)vendor, sizeof(vendor));
        libusb_close(tmp);

        if (serial[0] == '\0') {
            snprintf(serial, sizeof(serial), "%04X:%04X:%08X",
                     dd.idVendor, dd.idProduct, (unsigned)count);
        }

        if (count >= capacity) {
            capacity *= 2;
            nrf_probe_t *tmp2 = realloc(list, capacity * sizeof(nrf_probe_t));
            if (!tmp2) { free(list); libusb_free_device_list(ldevs, 1); return NRF_OCD_ERR_MEMORY; }
            list = tmp2;
        }
        nrf_probe_t *probe = &list[count];
        memset(probe, 0, sizeof(*probe));
        probe->vid = dd.idVendor;
        probe->pid = dd.idProduct;
        probe->report_in_size = 64;
        probe->report_out_size = 64;
        probe->is_v2 = true;  /* v2-only, no HID path */
        probe->path[0] = '\0';  /* no HID path for v2-only */
        copy_str(probe->serial, sizeof(probe->serial), serial);
        copy_str(probe->vendor, sizeof(probe->vendor), vendor);
        copy_str(probe->product, sizeof(probe->product), product);
        if (probe->vendor[0] == '\0') snprintf(probe->vendor, sizeof(probe->vendor), "%04X", dd.idVendor);
        if (probe->product[0] == '\0') snprintf(probe->product, sizeof(probe->product), "%04X", dd.idProduct);
        count++;
    }
    libusb_free_device_list(ldevs, 1);

    if (count == 0) { free(list); *out_list = NULL; *out_count = 0; return NRF_OCD_OK; }
    *out_list = list; *out_count = count;
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_probe_open(nrf_probe_t *probe) {
    hid_device *hdev = NULL;

    /* v2-only probes (flagged by enum) skip HID entirely */
    if (probe->is_v2) {
        probe->report_in_size = 64;
        probe->report_out_size = 64;
        return open_libusb_bulk_probe(probe, 64);
    }

    /* v1 probes: open via HID first */
    if (probe->path[0])
        hdev = hid_open_path(probe->path);
    if (!hdev)
        hdev = hid_open(probe->vid, probe->pid, NULL);
    if (!hdev) return NRF_OCD_ERR_USB_OPEN;

    unsigned char desc[4096];
    int desc_len = hid_get_report_descriptor(hdev, desc, sizeof(desc));

    /* CMSIS-DAP v1 HID uses fixed 64-byte reports. */
    int report_bytes = 64;
    bool is_v2 = false;
    (void)desc_len;

    probe->report_in_size = report_bytes;
    probe->report_out_size = report_bytes;
    probe->is_v2 = is_v2;

    /* Seeed XIAO nRF54: use v2 bulk interface (reliable, non-batching CMSIS-DAP v2).
     * The v1 HID interface batches multiple responses into a single report,
     * which is non-standard. */
    bool use_libusb = (probe->vid == 0x2886);

    if (use_libusb || probe->is_v2) {
        hid_close(hdev);
        probe->is_v2 = true;
        if (use_libusb) {
            probe->report_in_size = 64;
            probe->report_out_size = 64;
        }

        int packet_size = probe->report_in_size > 0 ? probe->report_in_size : 64;
        return open_libusb_bulk_probe(probe, packet_size);
    } else {
        probe->hid_handle = hdev;

        /* Flush any stale HID reports that may be buffered from
         * previous enumeration or device activity. */
        {
            uint8_t discard[256];
            int flushed = 0;
            /* Read up to 20 stale reports with a short timeout. */
            while (flushed < 20) {
                int r = hid_read_timeout(hdev, discard, sizeof(discard), 10);
                if (r <= 0) break;
                flushed++;
            }
            if (flushed > 0)
                NRF_DBG("Flushed %d stale HID report(s)", flushed);
        }
    }
    return NRF_OCD_OK;
}

void nrf_probe_close(nrf_probe_t *probe) {
    if (!probe || !probe->hid_handle) return;
    if (probe->is_v2) {
        v2_ctx_t *ctx = (v2_ctx_t *)probe->hid_handle;
        libusb_release_interface(ctx->handle, ctx->iface_num);
        libusb_attach_kernel_driver(ctx->handle, ctx->iface_num);
        libusb_close(ctx->handle);
        free(ctx);
    } else {
        hid_close((hid_device *)probe->hid_handle);
    }
    probe->hid_handle = NULL;
}

void nrf_probe_free_list(nrf_probe_t **list, int count) {
    (void)count; if (list && *list) { free(*list); *list = NULL; }
}

nrf_ocd_error_t nrf_probe_write(nrf_probe_t *probe, const uint8_t *data, int len) {
    if (!probe || !probe->hid_handle) return NRF_OCD_ERR_USB_OPEN;
    if (!data || len < 0) return NRF_OCD_ERR_USB_WRITE;

    if (probe->is_v2) {
        v2_ctx_t *ctx = (v2_ctx_t *)probe->hid_handle;
        if (len > ctx->packet_size)
            return NRF_OCD_ERR_USB_WRITE;
        uint8_t *buf = malloc(ctx->packet_size);
        if (!buf) return NRF_OCD_ERR_MEMORY;
        memset(buf, 0, ctx->packet_size);
        memcpy(buf, data, (size_t)len);
        int transferred = 0;
        int r = libusb_bulk_transfer(ctx->handle, ctx->ep_out, buf, ctx->packet_size, &transferred, 10000);
        free(buf);
        if (r < 0 || transferred != ctx->packet_size) return NRF_OCD_ERR_USB_WRITE;
    } else {
        hid_device *dev = (hid_device *)probe->hid_handle;
        int rs = probe->report_out_size;
        if (len > rs)
            return NRF_OCD_ERR_USB_WRITE;
        uint8_t *buf = malloc(rs + 1);
        if (!buf) return NRF_OCD_ERR_MEMORY;
        buf[0] = 0x00;
        memset(buf + 1, 0, rs);
        memcpy(buf + 1, data, (size_t)len);
        /* CMSIS-DAP v1 HID report = 1 report-ID byte + 63 data bytes = 64 total.
         * hid_write expects the length including the report ID. */
        int ret = hid_write(dev, buf, rs);
        free(buf);
        if (ret < 0) return NRF_OCD_ERR_USB_WRITE;
    }
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_probe_read(nrf_probe_t *probe, uint8_t *buf, int buf_size, int *out_len) {
    if (!probe || !probe->hid_handle) return NRF_OCD_ERR_USB_OPEN;
    if (!buf || !out_len || buf_size <= 0) return NRF_OCD_ERR_USB_READ;

    if (probe->is_v2) {
        v2_ctx_t *ctx = (v2_ctx_t *)probe->hid_handle;
        uint8_t *tmp = malloc(ctx->packet_size);
        if (!tmp) return NRF_OCD_ERR_MEMORY;
        int transferred = 0;
        int r = libusb_bulk_transfer(ctx->handle, ctx->ep_in, tmp, ctx->packet_size, &transferred, 10000);
        if (r >= 0 && transferred > 0) {
            int copy = transferred > buf_size ? buf_size : transferred;
            memcpy(buf, tmp, (size_t)copy);
            transferred = copy;
        }
        free(tmp);
        if (r < 0 || transferred <= 0) return NRF_OCD_ERR_USB_READ;
        *out_len = transferred;
    } else {
        hid_device *dev = (hid_device *)probe->hid_handle;
        int rs = probe->report_in_size;
        uint8_t *tmp = malloc(rs);
        if (!tmp) return NRF_OCD_ERR_MEMORY;
        int ret = hid_read_timeout(dev, tmp, rs, 10000);
        if (ret < 0) {
            free(tmp);
            return NRF_OCD_ERR_USB_READ;
        }
        if (ret > 0) {
            /* HID always prepends a report ID byte; strip it. */
            int offset = 1;
            int payload = ret - offset;
            if (payload > buf_size)
                payload = buf_size;
            memcpy(buf, tmp + offset, (size_t)payload);
            ret = payload;
        }
        free(tmp);
        *out_len = ret;
    }
    return NRF_OCD_OK;
}
