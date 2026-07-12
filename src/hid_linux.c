/* hid_linux.c - HID access via /dev/hidraw on Linux.
 *
 * Uses only the kernel's hidraw interface plus a couple of ioctls declared
 * in <linux/hidraw.h>. No libusb, no libhidapi. The probe enumeration walks
 * /sys/class/hidraw and follows the parent USB device to read idVendor /
 * idProduct / serial.
 *
 * Why hidraw and not libusb: libusb would let us also speak the bulk-only
 * CMSIS-DAP v2 transport, but we still need raw HID for v1. Going through
 * hidraw keeps the dependency footprint at zero and matches the surface
 * the macOS / Windows paths expose.
 */
#include "hid.h"
#include "hid_descriptor.h"
#include "log.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/types.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Use the kernel's struct hidraw_report_descriptor; do not redefine. */
#define HIDRAW_IOCTL_GET_REPORT_DESC _IOR('H', 0x02, struct hidraw_report_descriptor)

/* ----- Internal structures ------------------------------------------------- */
struct hid_device {
    int      fd;
    char     path[512];
    char     serial[64];
    uint16_t vid;
    uint16_t pid;
    int      report_size; /* negotiated via descriptor walk; fallback 64. */
};

struct hid_enumerate_handle {
    DIR *dir;
    int  index;
    int  count;
    hid_device_info_t *items;
    hid_device_info_t current;
};

/* ----- Read a sysfs file safely ------------------------------------------- */
static char *read_sysfs_file(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n <= 0) return NULL;
    out[n] = '\0';
    /* Trim trailing whitespace including newline. */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == ' ' || out[n-1] == '\t' || out[n-1] == '\0')) {
        out[--n] = '\0';
    }
    return out;
}

static int probe_report_size(int fd) {
    struct hidraw_report_descriptor rpt;
    rpt.size = HID_MAX_DESCRIPTOR_SIZE;
    if (ioctl(fd, HIDRAW_IOCTL_GET_REPORT_DESC, &rpt) < 0) {
        return 64;
    }
    return hid_input_report_size(rpt.value, rpt.size, 64);
}

/* ----- Public API ---------------------------------------------------------- */
nrf_ocd_status_t hid_init(void) { return NRF_OCD_OK; }
void             hid_shutdown(void) {}

static void free_enumerate_items(hid_enumerate_handle_t *h) {
    if (!h) return;
    for (int i = 0; i < h->count; i++) {
        /* All items are POD, no nested allocations. */
    }
    free(h->items);
    h->items = NULL;
    h->count = 0;
}

static int build_entry(const char *hidraw_name, hid_device_info_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "/dev/%s", hidraw_name);

    char vbuf[16], pbuf[16];
    char devpath[256];
    snprintf(devpath, sizeof(devpath), "/sys/class/hidraw/%s/device", hidraw_name);

    /* Resolve the real sysfs path of the HID device. */
    char resolved_path[512];
    ssize_t rl = readlink(devpath, resolved_path, sizeof(resolved_path) - 1);
    if (rl <= 0) return 0;
    resolved_path[rl] = '\0';

    /* Construct the absolute path. devpath looks like
     *   /sys/class/hidraw/hidraw3/device
     * and the link target is relative, e.g. ../../../devices/.../... .
     * Resolve to absolute by combining /sys/class/hidraw/hidraw3/ with
     * the link target. */
    char hidbase[256];
    snprintf(hidbase, sizeof(hidbase), "/sys/class/hidraw/%s", hidraw_name);
    char abs_hid[PATH_MAX];
    {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/%s", hidbase, resolved_path);
        char *p = realpath(tmp, abs_hid);
        if (!p) return 0;
    }

    /* Walk up to find the USB device (with idVendor / idProduct). The
     * chain is HID device -> USB interface -> USB device. */
    char cur[512];
    nrf_ocd_str_copy(cur, sizeof(cur), abs_hid);
    bool found = false;
    /* Bound the search to a few levels. */
    for (int depth = 0; depth < 8; depth++) {
        char idpath[768];
        snprintf(idpath, sizeof(idpath), "%s/idVendor", cur);
        if (read_sysfs_file(idpath, vbuf, sizeof(vbuf))) {
            snprintf(idpath, sizeof(idpath), "%s/idProduct", cur);
            if (read_sysfs_file(idpath, pbuf, sizeof(pbuf))) {
                out->vendor_id  = (uint16_t)strtoul(vbuf, NULL, 16);
                out->product_id = (uint16_t)strtoul(pbuf, NULL, 16);
                found = true;
                break;
            }
        }
        /* Strip last component. */
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
    }
    if (!found) {
        /* Fallback: parse the sysfs directory name itself. The HID device
         * directory is named e.g. "0003:2886:0066.04FD" and contains
         * the VID:PID. */
        const char *base = strrchr(abs_hid, '/');
        if (base) {
            const char *p = base + 1;
            /* Expect "XXXX:VVVV:PPPP.IIII". */
            if (strlen(p) > 9 && p[4] == ':' && p[9] == ':') {
                out->vendor_id  = (uint16_t)strtoul(p + 5, NULL, 16);
                out->product_id = (uint16_t)strtoul(p + 10, NULL, 16);
                found = true;
            }
        }
    }
    if (!found) return 0;

    /* Serial / product / manufacturer. They live in the USB device
     * directory, which is two levels above the HID device. */
    /* Reset cur to the USB device level by walking up from the
     * interface. The HID device's parent is the interface, whose parent
     * is the USB device. */
    char usb_dev[512] = {0};
    nrf_ocd_str_copy(usb_dev, sizeof(usb_dev), abs_hid);
    for (int i = 0; i < 2; i++) {
        char *slash = strrchr(usb_dev, '/');
        if (!slash || slash == usb_dev) break;
        *slash = '\0';
    }

    char pth[768];
    snprintf(pth, sizeof(pth), "%s/serial", usb_dev);
    read_sysfs_file(pth, out->serial_number, sizeof(out->serial_number));
    snprintf(pth, sizeof(pth), "%s/product", usb_dev);
    read_sysfs_file(pth, out->product_string, sizeof(out->product_string));
    snprintf(pth, sizeof(pth), "%s/manufacturer", usb_dev);
    read_sysfs_file(pth, out->manufacturer, sizeof(out->manufacturer));

    /* Interface number. */
    char intfpath[768];
    char intf_dir[512] = {0};
    nrf_ocd_str_copy(intf_dir, sizeof(intf_dir), abs_hid);
    {
        char *slash = strrchr(intf_dir, '/');
        if (slash && slash != intf_dir) *slash = '\0';
    }
    snprintf(intfpath, sizeof(intfpath), "%s/bInterfaceNumber", intf_dir);
    char ibuf[8];
    if (read_sysfs_file(intfpath, ibuf, sizeof(ibuf))) {
        out->interface_number = atoi(ibuf);
    } else {
        out->interface_number = 0;
    }

    /* Open the device briefly to read the raw HID name. */
    int fd = open(out->path, O_RDWR | O_NONBLOCK);
    if (fd >= 0) {
        char name[256] = {0};
        if (ioctl(fd, HIDIOCGRAWNAME(256), name) >= 0) {
            nrf_ocd_str_rstrip(name);
            if (!out->product_string[0] && name[0]) {
                size_t nl = strlen(name);
                if (nl >= sizeof(out->product_string)) nl = sizeof(out->product_string) - 1;
                memcpy(out->product_string, name, nl);
                out->product_string[nl] = '\0';
            }
        }
        close(fd);
    }
    return 1;
}

hid_enumerate_handle_t *hid_enumerate_start(void) {
    hid_enumerate_handle_t *h = (hid_enumerate_handle_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->dir = opendir("/sys/class/hidraw");
    if (!h->dir) {
        free(h);
        return NULL;
    }

    /* First pass: count eligible nodes. */
    int cap = 16;
    h->items = (hid_device_info_t *)calloc((size_t)cap, sizeof(hid_device_info_t));
    if (!h->items) {
        closedir(h->dir);
        free(h);
        return NULL;
    }

    struct dirent *de;
    while ((de = readdir(h->dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;
        if (h->count >= cap) {
            cap *= 2;
            hid_device_info_t *p = (hid_device_info_t *)realloc(h->items, (size_t)cap * sizeof(*p));
            if (!p) break;
            h->items = p;
        }
        if (build_entry(de->d_name, &h->items[h->count])) {
            h->count++;
        }
    }
    rewinddir(h->dir);
    h->index = 0;
    return h;
}

const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h) {
    if (!h) return NULL;
    while (h->index < h->count) {
        const hid_device_info_t *info = &h->items[h->index++];
        /* Skip interfaces that have no useful VID/PID. */
        if (info->vendor_id == 0 && info->product_id == 0) continue;
        memcpy(&h->current, info, sizeof(h->current));
        return &h->current;
    }
    return NULL;
}

void hid_enumerate_free(hid_enumerate_handle_t *h) {
    if (!h) return;
    if (h->dir) closedir(h->dir);
    free_enumerate_items(h);
    free(h);
}

const hid_device_info_t *hid_find_by_serial(const char *serial) {
    if (!serial) return NULL;
    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return NULL;
    const hid_device_info_t *info;
    while ((info = hid_enumerate_next(h)) != NULL) {
        if (nrf_ocd_strcasecmp(info->serial_number, serial) == 0) {
            /* Caller would have to copy; we don't keep a cache here. */
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

hid_device_t *hid_open_path(const char *path) {
    if (!path) return NULL;
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES) {
            LOG_ERROR("Permission denied opening %s. Hint: install a udev rule "
                      "or run as root.", path);
        } else {
            LOG_ERROR("open(%s) failed: %s", path, strerror(errno));
        }
        return NULL;
    }
    hid_device_t *dev = (hid_device_t *)calloc(1, sizeof(*dev));
    if (!dev) {
        close(fd);
        return NULL;
    }
    dev->fd = fd;
    nrf_ocd_str_copy(dev->path, sizeof(dev->path), path);
    dev->report_size = probe_report_size(fd);

    struct hidraw_devinfo info;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        dev->vid = (uint16_t)info.vendor;
        dev->pid = (uint16_t)info.product;
    }
    char name[256] = {0};
    if (ioctl(fd, HIDIOCGRAWNAME(256), name) >= 0) {
        nrf_ocd_str_rstrip(name);
    }
    /* Serial is not always queryable; prefer sysfs lookup. */
    char pth[512];
    snprintf(pth, sizeof(pth), "/sys/class/hidraw/%s/device/serial",
             strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    char sbuf[64] = {0};
    if (read_sysfs_file(pth, sbuf, sizeof(sbuf))) {
        nrf_ocd_str_copy(dev->serial, sizeof(dev->serial), sbuf);
    }
    return dev;
}

void hid_close(hid_device_t *dev) {
    if (!dev) return;
    if (dev->fd >= 0) close(dev->fd);
    free(dev);
}

bool hid_has_report(hid_device_t *dev) {
    if (!dev) return false;
    struct pollfd pfd = { .fd = dev->fd, .events = POLLIN };
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms) {
    if (!dev || !buf) return NRF_OCD_ERR_INVALID_ARG;
    /* Zero the buffer so leftover data from previous reads does not
     * confuse the protocol parser. The hidraw read only returns the
     * bytes the device actually sent; the rest of the buffer is
     * undefined and may still hold data from the previous transfer. */
    memset(buf, 0, buf_size);
    struct pollfd pfd = { .fd = dev->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return NRF_OCD_ERR_TIMEOUT;
    if (pr < 0)  return NRF_OCD_ERR_IO;
    ssize_t n = read(dev->fd, buf, buf_size);
    if (n < 0) {
        if (errno == EAGAIN) return NRF_OCD_ERR_TIMEOUT;
        return NRF_OCD_ERR_IO;
    }
    if (out_len) *out_len = (size_t)n;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    if (!dev || !data) return NRF_OCD_ERR_INVALID_ARG;
    struct pollfd pfd = { .fd = dev->fd, .events = POLLOUT };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return NRF_OCD_ERR_TIMEOUT;
    if (pr < 0)  return NRF_OCD_ERR_IO;
    ssize_t n = write(dev->fd, data, len);
    if (n < 0) {
        if (errno == EAGAIN) return NRF_OCD_ERR_TIMEOUT;
        return NRF_OCD_ERR_IO;
    }
    return NRF_OCD_OK;
}

void hid_flush(hid_device_t *dev) {
    (void)dev;
}

const char *hid_path(const hid_device_t *dev)     { return dev ? dev->path : ""; }
const char *hid_serial(const hid_device_t *dev)   { return dev ? dev->serial : ""; }
uint16_t    hid_vid(const hid_device_t *dev)      { return dev ? dev->vid : 0; }
uint16_t    hid_pid(const hid_device_t *dev)      { return dev ? dev->pid : 0; }
int         hid_report_size(const hid_device_t *dev) { return dev ? dev->report_size : 64; }
void        hid_mark_bulk(hid_device_t *dev)        { (void)dev; }
bool        hid_is_bulk(const hid_device_t *dev)   { (void)dev; return false; }
