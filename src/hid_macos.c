/* hid_macos.c - HID access via IOKit on macOS.
 *
 * Uses IOHIDManager to enumerate HID devices and open them. We filter by
 * vendor/product id; the path stored in hid_device_info_t is the
 * IOHIDDevice refcon encoded as a string (e.g. "ioservice:0x1234").
 *
 * macOS exposes the CMSIS-DAP HID interface through IOHIDManager the same
 * way the kernel exposes it on Linux, so the rest of the code does not
 * need to know which OS is running.
 */
#include "hid.h"
#include "log.h"
#include "util.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct hid_device {
    IOHIDDeviceRef device;
    char     path[64];
    char     serial[64];
    uint16_t vid;
    uint16_t pid;
    int      report_size;
    pthread_mutex_t report_lock;
    pthread_cond_t  report_ready;
    bool     have_report;
    uint8_t  last_report[NRF_OCD_HID_REPORT_SIZE];
    size_t   last_len;
};

struct hid_enumerate_handle {
    IOHIDManagerRef mgr;
    CFSetRef        devices;
    CFIndex         index;
    hid_device_info_t *items;
    int              count;
    hid_device_info_t current;
};

/* ----- Refcon / IOHIDDevice helpers --------------------------------------- */
static void store_cfstr(CFStringRef cf, char *dst, size_t cap) {
    if (!cf || cap == 0) {
        if (cap > 0) dst[0] = '\0';
        return;
    }
    CFIndex len = CFStringGetLength(cf);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8);
    if (max + 1 > (CFIndex)cap) max = (CFIndex)cap - 1;
    CFStringGetCString(cf, dst, (CFIndex)cap, kCFStringEncodingUTF8);
}

static int get_int_property(IOHIDDeviceRef dev, CFStringRef key) {
    CFNumberRef n = IOHIDDeviceGetProperty(dev, key);
    if (!n) return 0;
    int v = 0;
    CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

static CFStringRef get_str_property(IOHIDDeviceRef dev, CFStringRef key) {
    return (CFStringRef)IOHIDDeviceGetProperty(dev, key);
}

/* HID report callback. Pushes a report into the device. */
static void hid_report_callback(void *ctx, IOReturn result, void *sender,
                                IOHIDReportType type, uint32_t reportID,
                                const uint8_t *report, CFIndex reportLength) {
    (void)result; (void)sender; (void)type;
    hid_device_t *dev = (hid_device_t *)ctx;
    if (!dev || reportLength <= 0 || reportLength > (CFIndex)sizeof(dev->last_report)) {
        return;
    }
    pthread_mutex_lock(&dev->report_lock);
    memcpy(dev->last_report, report, (size_t)reportLength);
    /* macOS IOHID report callback strips the report id byte. Restore it. */
    if (reportID != 0) {
        memmove(dev->last_report + 1, dev->last_report, (size_t)reportLength);
        dev->last_report[0] = (uint8_t)reportID;
        dev->last_len = (size_t)reportLength + 1;
    } else {
        dev->last_len = (size_t)reportLength;
    }
    dev->have_report = true;
    pthread_cond_signal(&dev->report_ready);
    pthread_mutex_unlock(&dev->report_lock);
}

/* ----- Public API ---------------------------------------------------------- */
nrf_ocd_status_t hid_init(void) { return NRF_OCD_OK; }
void             hid_shutdown(void) {}

hid_enumerate_handle_t *hid_enumerate_start(void) {
    hid_enumerate_handle_t *h = (hid_enumerate_handle_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!h->mgr) { free(h); return NULL; }
    /* We use no matching filters - we'll iterate every HID device and let
     * hid_enumerate_next() apply our pid/vid filter. */
    IOHIDManagerSetDeviceMatching(h->mgr, NULL);
    IOHIDManagerOpen(h->mgr, kIOHIDOptionsTypeNone);
    h->devices = IOHIDManagerCopyDevices(h->mgr);
    if (!h->devices) {
        CFRelease(h->mgr);
        free(h);
        return NULL;
    }
    h->index = 0;
    h->items = NULL;
    h->count = 0;
    h->current.serial_number[0] = '\0';
    h->current.product_string[0] = '\0';
    h->current.manufacturer[0] = '\0';
    h->current.path[0] = '\0';
    return h;
}

const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h) {
    if (!h || !h->devices) return NULL;
    CFIndex n = CFSetGetCount(h->devices);
    if (n <= 0) return NULL;
    if (!h->items) {
        h->items = (hid_device_info_t *)calloc((size_t)n, sizeof(hid_device_info_t));
        if (!h->items) return NULL;
    }
    CFTypeRef *arr = (CFTypeRef *)calloc((size_t)n, sizeof(CFTypeRef));
    if (!arr) return NULL;
    CFSetGetValues(h->devices, arr);
    while (h->index < n) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)arr[h->index++];
        if (!dev) continue;
        int vid = get_int_property(dev, CFSTR(kIOHIDVendorIDKey));
        int pid = get_int_property(dev, CFSTR(kIOHIDProductIDKey));
        if (vid == 0 && pid == 0) continue;
        hid_device_info_t *info = &h->items[h->count++];
        memset(info, 0, sizeof(*info));
        info->vendor_id  = (uint16_t)vid;
        info->product_id = (uint16_t)pid;
        store_cfstr(get_str_property(dev, CFSTR(kIOHIDSerialNumberKey)),
                    info->serial_number, sizeof(info->serial_number));
        store_cfstr(get_str_property(dev, CFSTR(kIOHIDProductKey)),
                    info->product_string, sizeof(info->product_string));
        store_cfstr(get_str_property(dev, CFSTR(kIOHIDManufacturerKey)),
                    info->manufacturer, sizeof(info->manufacturer));
        /* IOHIDLocationID as a path proxy. */
        int loc = get_int_property(dev, CFSTR(kIOHIDLocationIDKey));
        snprintf(info->path, sizeof(info->path), "ioservice:0x%08x", (unsigned)loc);
        info->usage_page = (uint16_t)get_int_property(dev, CFSTR(kIOHIDPrimaryUsagePageKey));
        info->usage      = (uint16_t)get_int_property(dev, CFSTR(kIOHIDPrimaryUsageKey));
        info->is_dap_v2  = false;
        free(arr);
        memcpy(&h->current, info, sizeof(h->current));
        return &h->current;
    }
    free(arr);
    return NULL;
}

void hid_enumerate_free(hid_enumerate_handle_t *h) {
    if (!h) return;
    if (h->devices) CFRelease(h->devices);
    if (h->mgr)     CFRelease(h->mgr);
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

/* On macOS the path field is a placeholder; we re-enumerate and match by
 * the printed path prefix. */
static IOHIDDeviceRef find_device_by_path(const char *path) {
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info;
    IOHIDDeviceRef dev = NULL;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (nrf_ocd_strcasecmp(info->path, path) == 0) {
            /* Need to fetch the IOHIDDeviceRef. We saved them in items,
             * but those are not exposed. As a workaround we use
             * IOHIDManagerCopyDevices and walk it again. */
            break;
        }
    }
    if (info) {
        /* Re-walk to find the matching device. */
        CFSetRef devices = IOHIDManagerCopyDevices(h->mgr);
        if (devices) {
            CFIndex n = CFSetGetCount(devices);
            CFTypeRef *arr = (CFTypeRef *)calloc((size_t)n, sizeof(CFTypeRef));
            CFSetGetValues(devices, arr);
            for (CFIndex i = 0; i < n; i++) {
                IOHIDDeviceRef d = (IOHIDDeviceRef)arr[i];
                int loc = get_int_property(d, CFSTR(kIOHIDLocationIDKey));
                char buf[64];
                snprintf(buf, sizeof(buf), "ioservice:0x%08x", (unsigned)loc);
                if (nrf_ocd_strcasecmp(buf, path) == 0) {
                    dev = (IOHIDDeviceRef)CFRetain(d);
                    break;
                }
            }
            free(arr);
            CFRelease(devices);
        }
    }
    hid_enumerate_free(h);
    return dev;
}

hid_device_t *hid_open_path(const char *path) {
    if (!path) return NULL;
    IOHIDDeviceRef dev = find_device_by_path(path);
    if (!dev) {
        LOG_ERROR("macOS: device not found for path %s", path);
        return NULL;
    }
    hid_device_t *d = (hid_device_t *)calloc(1, sizeof(*d));
    if (!d) { CFRelease(dev); return NULL; }
    d->device = dev;
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->vid = (uint16_t)get_int_property(dev, CFSTR(kIOHIDVendorIDKey));
    d->pid = (uint16_t)get_int_property(dev, CFSTR(kIOHIDProductIDKey));
    store_cfstr(get_str_property(dev, CFSTR(kIOHIDSerialNumberKey)),
                d->serial, sizeof(d->serial));
    /* Report size: standard 64. v2 will return 64 anyway. */
    d->report_size = 64;
    pthread_mutex_init(&d->report_lock, NULL);
    pthread_cond_init(&d->report_ready, NULL);

    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        LOG_ERROR("IOHIDDeviceOpen failed: 0x%x", r);
        CFRelease(dev);
        pthread_mutex_destroy(&d->report_lock);
        pthread_cond_destroy(&d->report_ready);
        free(d);
        return NULL;
    }
    IOHIDDeviceRegisterInputReportCallback(
        dev, (uint8_t *)d->last_report, sizeof(d->last_report),
        hid_report_callback, d);
    /* Schedule with the run loop. */
    IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    return d;
}

void hid_close(hid_device_t *dev) {
    if (!dev) return;
    if (dev->device) {
        IOHIDDeviceUnscheduleFromRunLoop(dev->device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        IOHIDDeviceClose(dev->device, kIOHIDOptionsTypeNone);
        CFRelease(dev->device);
    }
    pthread_mutex_destroy(&dev->report_lock);
    pthread_cond_destroy(&dev->report_ready);
    free(dev);
}

bool hid_has_report(hid_device_t *dev) {
    if (!dev) return false;
    pthread_mutex_lock(&dev->report_lock);
    bool ready = dev->have_report;
    pthread_mutex_unlock(&dev->report_lock);
    return ready;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms) {
    if (!dev || !buf) return NRF_OCD_ERR_INVALID_ARG;
    pthread_mutex_lock(&dev->report_lock);
    if (!dev->have_report) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += 1;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&dev->report_ready, &dev->report_lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&dev->report_lock);
            return NRF_OCD_ERR_TIMEOUT;
        }
        if (rc != 0) {
            pthread_mutex_unlock(&dev->report_lock);
            return NRF_OCD_ERR_IO;
        }
    }
    size_t n = dev->last_len;
    if (n > buf_size) n = buf_size;
    memcpy(buf, dev->last_report, n);
    dev->have_report = false;
    pthread_mutex_unlock(&dev->report_lock);
    if (out_len) *out_len = n;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    if (!dev || !dev->device || !data) return NRF_OCD_ERR_INVALID_ARG;
    (void)timeout_ms;
    uint8_t report_id = data[0];
    const uint8_t *body = data + 1;
    CFIndex body_len = (CFIndex)(len - 1);
    IOReturn r = IOHIDDeviceSetReport(dev->device, kIOHIDReportTypeOutput,
                                      report_id, body, body_len);
    if (r != kIOReturnSuccess) return NRF_OCD_ERR_IO;
    return NRF_OCD_OK;
}

const char *hid_path(const hid_device_t *dev)     { return dev ? dev->path : ""; }
const char *hid_serial(const hid_device_t *dev)   { return dev ? dev->serial : ""; }
uint16_t    hid_vid(const hid_device_t *dev)      { return dev ? dev->vid : 0; }
uint16_t    hid_pid(const hid_device_t *dev)      { return dev ? dev->pid : 0; }
int         hid_report_size(const hid_device_t *dev) { return dev ? dev->report_size : 64; }
