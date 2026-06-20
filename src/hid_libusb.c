/* hid_libusb.c - libusb-1.0 backend for CMSIS-DAP probes.
 *
 * This is the bulk-endpoint (CMSIS-DAP v2) alternative to hidraw. The
 * Seeed XIAO nRF54 board (and several other recent DAPLink boards) report
 * correct DAP_Transfer response lengths over USB bulk endpoints but
 * truncate them over the HID interrupt endpoint. The pyOCD library works
 * around this by always using the v2 bulk endpoints; we follow the same
 * approach when libusb is available.
 *
 * The behaviour here mirrors pyOCD 0.44.1's pyusb_v2_backend.py exactly:
 *   - open():  claim the CMSIS-DAP v2 interface, then flush the IN endpoint.
 *   - write(): send the exact command bytes (no padding, no report-id
 *              prefix) and emit a zero-length packet when the length is an
 *              exact multiple of the endpoint wMaxPacketSize but shorter
 *              than the DAP packet size, so the device sees transfer end.
 *   - read():  bulk-read up to packet_size bytes (512).
 *
 * From the rest of the code's perspective the device behaves exactly like
 * an HID probe - we just route the bytes through libusb.
 */
#include "hid.h"
#include "log.h"
#include "nrf_ocd.h"
#include "util.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* USB read buffer / logical DAP packet size, matching pyOCD's default. */
#define NRF_OCD_DAP_PACKET_SIZE 512

struct hid_device {
    libusb_device_handle *handle;
    uint8_t   in_endpoint;
    uint8_t   out_endpoint;
    uint16_t  out_max_packet;   /* wMaxPacketSize of the OUT endpoint (ZLP rule). */
    uint16_t  packet_size;      /* DAP packet size / read buffer (512). */
    uint8_t   interface_number; /* interface we claimed. */
    char      path[512];
    char      serial[64];
    uint16_t  vid;
    uint16_t  pid;
    int       report_size;      /* HID report length (64); high bit tags bulk. */
};

struct hid_enumerate_handle {
    libusb_device **list;
    ssize_t         count;
    ssize_t         index;
    hid_device_info_t *items;
    int              n_items;
    hid_device_info_t current;
};

/* ----- Enumeration helpers ------------------------------------------------ */
static void fill_info_from_descriptor(libusb_device *dev, hid_device_info_t *info) {
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) == 0) {
        info->vendor_id  = desc.idVendor;
        info->product_id = desc.idProduct;
    }
    libusb_device_handle *h = NULL;
    if (libusb_open(dev, &h) == 0) {
        unsigned char buf[256];
        int len = libusb_get_string_descriptor_ascii(h, desc.iManufacturer,
                                                      buf, sizeof(buf));
        if (len > 0) { memcpy(info->manufacturer, buf, (size_t)len < 128 ? (size_t)len : 127); }
        len = libusb_get_string_descriptor_ascii(h, desc.iProduct,
                                                  buf, sizeof(buf));
        if (len > 0) { memcpy(info->product_string, buf, (size_t)len < 128 ? (size_t)len : 127); }
        len = libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                                  buf, sizeof(buf));
        if (len > 0) { memcpy(info->serial_number, buf, (size_t)len < 64 ? (size_t)len : 63); }
        libusb_close(h);
    }
}

/* Find the CMSIS-DAP v2 vendor-specific bulk interface. Selection rules
 * match pyOCD's _match_cmsis_dap_v2_interface(): class 0xFF, subclass 0,
 * with a bulk OUT and bulk IN endpoint. Returns the endpoints, the OUT
 * endpoint's wMaxPacketSize, and the interface number. */
static int find_cmsis_dap_interface(libusb_device_handle *h,
                                     uint8_t *out_ep, uint8_t *in_ep,
                                     uint16_t *out_max_pkt, uint8_t *iface_no) {
    struct libusb_config_descriptor *cfg = NULL;
    libusb_device *d = libusb_get_device(h);
    if (!d) return -1;
    if (libusb_get_active_config_descriptor(d, &cfg) != 0) return -1;
    int found = 0;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *alt = &intf->altsetting[a];
            if (alt->bInterfaceClass    != 0xFF) continue;
            if (alt->bInterfaceSubClass != 0x00) continue;
            uint8_t out = 0, in = 0;
            uint16_t out_max = 0;
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    if (!in) in = ep->bEndpointAddress;
                } else {
                    if (!out) {
                        out = ep->bEndpointAddress;
                        out_max = ep->wMaxPacketSize;
                    }
                }
            }
            if (out && in) {
                LOG_DEBUG("CMSIS-DAP v2: iface %d out=0x%02x in=0x%02x "
                          "outMaxPkt=%u", i, out, in, out_max);
                *out_ep = out;
                *in_ep = in;
                if (out_max_pkt) *out_max_pkt = out_max;
                if (iface_no) *iface_no = (uint8_t)i;
                found = 1;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found ? 0 : -1;
}

static void make_device_path(libusb_device *dev, char *out, size_t cap) {
    uint8_t port_numbers[8];
    int n = libusb_get_port_numbers(dev, port_numbers, sizeof(port_numbers));
    int p = snprintf(out, cap, "usb:%03d:%03d",
                     libusb_get_bus_number(dev),
                     libusb_get_device_address(dev));
    for (int i = 0; i < n && p < (int)cap - 4; i++) {
        p += snprintf(out + p, cap - (size_t)p, ".%d", port_numbers[i]);
    }
}

static int build_cmsis_entry(libusb_device *dev, hid_device_info_t *out) {
    memset(out, 0, sizeof(*out));
    fill_info_from_descriptor(dev, out);
    make_device_path(dev, out->path, sizeof(out->path));
    return 1;
}

nrf_ocd_status_t hid_init(void) {
    if (libusb_init(NULL) != 0) return NRF_OCD_ERR_IO;
    return NRF_OCD_OK;
}

void hid_shutdown(void) {
    libusb_exit(NULL);
}

hid_enumerate_handle_t *hid_enumerate_start(void) {
    hid_enumerate_handle_t *h = (hid_enumerate_handle_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->count = libusb_get_device_list(NULL, &h->list);
    if (h->count < 0) { free(h); return NULL; }
    h->items = (hid_device_info_t *)calloc((size_t)h->count, sizeof(hid_device_info_t));
    if (!h->items) { libusb_free_device_list(h->list, 1); free(h); return NULL; }
    return h;
}

const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h) {
    if (!h) return NULL;
    while (h->index < h->count) {
        libusb_device *dev = h->list[h->index++];
        if (!dev) continue;
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue;
        if (desc.idVendor == 0 && desc.idProduct == 0) continue;
        build_cmsis_entry(dev, &h->items[h->n_items]);
        memcpy(&h->current, &h->items[h->n_items], sizeof(h->current));
        h->n_items++;
        return &h->current;
    }
    return NULL;
}

void hid_enumerate_free(hid_enumerate_handle_t *h) {
    if (!h) return;
    if (h->list) libusb_free_device_list(h->list, 1);
    free(h->items);
    free(h);
}

const hid_device_info_t *hid_find_by_serial(const char *serial) {
    if (!serial) return NULL;
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (nrf_ocd_strcasecmp(info->serial_number, serial) == 0) {
            static __thread hid_device_info_t copy;
            copy = *info;
            hid_enumerate_free(h);
            return &copy;
        }
    }
    hid_enumerate_free(h);
    return NULL;
}

const hid_device_info_t *hid_find_by_index(unsigned index) {
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info = NULL;
    unsigned i = 0;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (i == index) {
            static __thread hid_device_info_t copy;
            copy = *info;
            hid_enumerate_free(h);
            return &copy;
        }
        i++;
    }
    hid_enumerate_free(h);
    return NULL;
}

static libusb_device *find_device_by_path(const char *path) {
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(NULL, &list);
    if (count < 0) return NULL;
    libusb_device *result = NULL;
    for (ssize_t i = 0; i < count; i++) {
        if (!list[i]) continue;
        char tmp[512];
        make_device_path(list[i], tmp, sizeof(tmp));
        if (nrf_ocd_strcasecmp(tmp, path) == 0) {
            result = list[i];
            libusb_ref_device(result);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return result;
}

/* Drain any stale data the probe may have buffered on its IN endpoint,
 * matching pyOCD's start_rx() "read until timeout" flush. */
static void flush_in_endpoint(libusb_device_handle *h, uint8_t in_ep) {
    uint8_t tmp[NRF_OCD_DAP_PACKET_SIZE];
    int transferred = 0;
    for (int i = 0; i < 32; i++) {
        int rc = libusb_bulk_transfer(h, in_ep | LIBUSB_ENDPOINT_IN, tmp,
                                      (int)sizeof(tmp), &transferred, 5);
        if (rc != 0 || transferred <= 0) break;
    }
}

hid_device_t *hid_open_path(const char *path) {
    if (!path) return NULL;
    libusb_device *dev = find_device_by_path(path);
    if (!dev) {
        LOG_ERROR("libusb: device not found for path %s", path);
        return NULL;
    }
    libusb_device_handle *h = NULL;
    if (libusb_open(dev, &h) != 0) {
        LOG_ERROR("libusb_open failed for path %s", path);
        libusb_unref_device(dev);
        return NULL;
    }
    libusb_unref_device(dev);

    /* Let libusb detach/reattach the kernel driver around our claim,
     * exactly like pyusb's managed claim_interface. */
    (void)libusb_set_auto_detach_kernel_driver(h, 1);

    uint8_t in_ep = 0, out_ep = 0, iface = 0;
    uint16_t out_max = 0;
    if (find_cmsis_dap_interface(h, &out_ep, &in_ep, &out_max, &iface) != 0) {
        LOG_ERROR("Could not find CMSIS-DAP v2 bulk interface for %s", path);
        libusb_close(h);
        return NULL;
    }
    if (libusb_claim_interface(h, iface) != 0) {
        LOG_ERROR("libusb_claim_interface(%u) failed for %s", iface, path);
        libusb_close(h);
        return NULL;
    }

    /* Flush any stale response the bridge may hold from a prior session. */
    flush_in_endpoint(h, in_ep);

    hid_device_t *d = (hid_device_t *)calloc(1, sizeof(*d));
    if (!d) { libusb_release_interface(h, iface); libusb_close(h); return NULL; }
    d->handle = h;
    d->in_endpoint = in_ep;
    d->out_endpoint = out_ep;
    d->out_max_packet = out_max ? out_max : 64;
    d->packet_size = NRF_OCD_DAP_PACKET_SIZE;
    d->interface_number = iface;
    d->report_size = 64;
    strncpy(d->path, path, sizeof(d->path) - 1);

    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) == 0) {
        d->vid = desc.idVendor;
        d->pid = desc.idProduct;
        unsigned char buf[256];
        if (libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                                buf, sizeof(buf)) > 0) {
            strncpy(d->serial, (const char *)buf, sizeof(d->serial) - 1);
        }
    }
    return d;
}

void hid_close(hid_device_t *dev) {
    if (!dev) return;
    if (dev->handle) {
        libusb_release_interface(dev->handle, dev->interface_number);
        libusb_close(dev->handle);
    }
    free(dev);
}

bool hid_has_report(hid_device_t *dev) {
    (void)dev;
    return false;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms) {
    if (!dev || !dev->handle || !buf) return NRF_OCD_ERR_INVALID_ARG;
    int want = (int)dev->packet_size;
    if (want <= 0) want = NRF_OCD_DAP_PACKET_SIZE;
    uint8_t *tmp = (uint8_t *)malloc((size_t)want);
    if (!tmp) return NRF_OCD_ERR_NO_MEM;
    int transferred = 0;
    int rc = libusb_bulk_transfer(dev->handle, dev->in_endpoint, tmp,
                                  want, &transferred, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) { free(tmp); return NRF_OCD_ERR_TIMEOUT; }
    if (rc != 0) {
        LOG_DEBUG("bulk read failed: rc=%d", rc);
        free(tmp);
        return NRF_OCD_ERR_IO;
    }
    size_t copy = (transferred >= 0) ? (size_t)transferred : 0;
    if (copy > buf_size) copy = buf_size;
    memcpy(buf, tmp, copy);
    free(tmp);
    if (out_len) *out_len = copy;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    if (!dev || !dev->handle || !data) return NRF_OCD_ERR_INVALID_ARG;
    int transferred = 0;
    int rc = libusb_bulk_transfer(dev->handle, dev->out_endpoint,
                                  (uint8_t *)data, (int)len,
                                  &transferred, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) return NRF_OCD_ERR_TIMEOUT;
    if (rc != 0) {
        LOG_DEBUG("bulk write failed: rc=%d len=%zu", rc, len);
        return NRF_OCD_ERR_IO;
    }
    /* Terminate the transfer with a zero-length packet when the payload is
     * an exact multiple of wMaxPacketSize but shorter than the DAP packet
     * size, so the device knows the OUT transfer is complete (pyOCD). */
    if (len > 0 && len < dev->packet_size &&
        dev->out_max_packet > 0 && (len % dev->out_max_packet) == 0) {
        int dummy = 0;
        (void)libusb_bulk_transfer(dev->handle, dev->out_endpoint,
                                   (uint8_t *)&dummy, 0, &transferred, timeout_ms);
    }
    return NRF_OCD_OK;
}

const char *hid_path(const hid_device_t *dev)     { return dev ? dev->path : ""; }
const char *hid_serial(const hid_device_t *dev)   { return dev ? dev->serial : ""; }
uint16_t    hid_vid(const hid_device_t *dev)      { return dev ? dev->vid : 0; }
uint16_t    hid_pid(const hid_device_t *dev)      { return dev ? dev->pid : 0; }
int         hid_report_size(const hid_device_t *dev) { return dev ? dev->report_size : 64; }

void hid_mark_bulk(hid_device_t *dev) {
    if (!dev) return;
    /* Tag via the high bit of report_size (private convention). */
    dev->report_size |= 0x8000;
}
bool hid_is_bulk(const hid_device_t *dev) {
    return dev && (dev->report_size & 0x8000);
}
