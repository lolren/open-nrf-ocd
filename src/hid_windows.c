/* hid_windows.c - HID access via hid.dll + setupapi.dll on Windows.
 *
 * Uses HidD_GetAttributes / HidD_GetSerialNumberString /
 * HidD_GetProductString to enumerate probes, and CreateFile + ReadFile /
 * WriteFile to talk to them. Blocking reads with cancellation via an
 * OVERLAPPED pipe. We deliberately do not require any external library
 * (hidapi / libusb).
 */
#include "hid.h"
#include "log.h"
#include "util.h"

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Microsoft-defined GUID for HIDClass devices. */
static const GUID GUID_DEVINTERFACE_HID =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

struct hid_device {
    HANDLE   handle;
    OVERLAPPED read_ovl;
    bool     read_pending;
    uint8_t  last_report[NRF_OCD_HID_REPORT_SIZE];
    DWORD    last_len;
    char     path[512];
    char     serial[64];
    uint16_t vid;
    uint16_t pid;
    int      report_size;
};

struct hid_enumerate_handle {
    HDEVINFO dev_info;
    int      index;
    hid_device_info_t *items;
    int              count;
    hid_device_info_t current;
};

static void wstr_to_str(const wchar_t *w, char *out, size_t cap) {
    if (!w || cap == 0) { if (cap) out[0] = '\0'; return; }
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cap, NULL, NULL);
    if (cap > 0) out[cap - 1] = '\0';
}

/* ----- Public API ---------------------------------------------------------- */
nrf_ocd_status_t hid_init(void) { return NRF_OCD_OK; }
void             hid_shutdown(void) {}

hid_enumerate_handle_t *hid_enumerate_start(void) {
    hid_enumerate_handle_t *h = (hid_enumerate_handle_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->dev_info = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h->dev_info == INVALID_HANDLE_VALUE) {
        free(h);
        return NULL;
    }
    h->index = 0;
    h->items = NULL;
    h->count = 0;
    return h;
}

const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h) {
    if (!h) return NULL;
    if (!h->items) {
        /* First call: walk all HID interfaces and build entries. */
        SP_DEVICE_INTERFACE_DATA dev_data;
        dev_data.cbSize = sizeof(dev_data);
        int cap = 16;
        h->items = (hid_device_info_t *)calloc((size_t)cap, sizeof(hid_device_info_t));
        if (!h->items) return NULL;
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(h->dev_info, NULL,
                                                      &GUID_DEVINTERFACE_HID, i, &dev_data); i++) {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetail(h->dev_info, &dev_data, NULL, 0, &required, NULL);
            if (required == 0) continue;
            SP_DEVICE_INTERFACE_DETAIL_DATA *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(required);
            if (!detail) continue;
            detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA did = { .cbSize = sizeof(did) };
            if (!SetupDiGetDeviceInterfaceDetail(h->dev_info, &dev_data, detail, required, NULL, &did)) {
                free(detail);
                continue;
            }
            HANDLE h2 = CreateFile(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h2 == INVALID_HANDLE_VALUE) {
                free(detail);
                continue;
            }
            HIDD_ATTRIBUTES attr = { .Size = sizeof(attr) };
            if (!HidD_GetAttributes(h2, &attr)) {
                CloseHandle(h2);
                free(detail);
                continue;
            }
            if (h->count >= cap) {
                cap *= 2;
                hid_device_info_t *p = (hid_device_info_t *)realloc(h->items, (size_t)cap * sizeof(*p));
                if (!p) { CloseHandle(h2); free(detail); break; }
                h->items = p;
            }
            hid_device_info_t *info = &h->items[h->count++];
            memset(info, 0, sizeof(*info));
            info->vendor_id  = attr.VendorID;
            info->product_id = attr.ProductID;
            wstr_to_str(detail->DevicePath, info->path, sizeof(info->path));

            wchar_t buf[256];
            if (HidD_GetSerialNumberString(h2, buf, sizeof(buf)))
                wstr_to_str(buf, info->serial_number, sizeof(info->serial_number));
            if (HidD_GetProductString(h2, buf, sizeof(buf)))
                wstr_to_str(buf, info->product_string, sizeof(info->product_string));
            CloseHandle(h2);
            free(detail);
        }
    }

    while (h->index < h->count) {
        const hid_device_info_t *info = &h->items[h->index++];
        memcpy(&h->current, info, sizeof(h->current));
        return &h->current;
    }
    return NULL;
}

void hid_enumerate_free(hid_enumerate_handle_t *h) {
    if (!h) return;
    if (h->dev_info && h->dev_info != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(h->dev_info);
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

hid_device_t *hid_open_path(const char *path) {
    if (!path) return NULL;
    /* path is a wide-string DevicePath, but we stored it as char. Convert
     * back to wide. */
    wchar_t wpath[512];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 512);
    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateFileW failed for %s: %lu", path, GetLastError());
        return NULL;
    }
    hid_device_t *dev = (hid_device_t *)calloc(1, sizeof(*dev));
    if (!dev) { CloseHandle(h); return NULL; }
    dev->handle = h;
    strncpy(dev->path, path, sizeof(dev->path) - 1);

    HIDD_ATTRIBUTES attr = { .Size = sizeof(attr) };
    if (HidD_GetAttributes(h, &attr)) {
        dev->vid = attr.VendorID;
        dev->pid = attr.ProductID;
    }
    wchar_t buf[256];
    if (HidD_GetSerialNumberString(h, buf, sizeof(buf)))
        wstr_to_str(buf, dev->serial, sizeof(dev->serial));

    PHIDP_PREPARSED_DATA ppd = NULL;
    if (HidD_GetPreparsedData(h, &ppd)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            dev->report_size = caps.InputReportByteLength;
            if (dev->report_size > (int)sizeof(dev->last_report))
                dev->report_size = sizeof(dev->last_report);
        }
        HidD_FreePreparsedData(ppd);
    }
    if (dev->report_size <= 0) dev->report_size = 64;

    memset(&dev->read_ovl, 0, sizeof(dev->read_ovl));
    dev->read_ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    return dev;
}

void hid_close(hid_device_t *dev) {
    if (!dev) return;
    if (dev->read_ovl.hEvent) CloseHandle(dev->read_ovl.hEvent);
    if (dev->handle && dev->handle != INVALID_HANDLE_VALUE) CloseHandle(dev->handle);
    free(dev);
}

bool hid_has_report(hid_device_t *dev) {
    if (!dev) return false;
    return dev->read_pending ? WaitForSingleObject(dev->read_ovl.hEvent, 0) == WAIT_OBJECT_0 : false;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms) {
    if (!dev || !buf) return NRF_OCD_ERR_INVALID_ARG;
    if (!dev->read_pending) {
        if (!ReadFile(dev->handle, dev->last_report, sizeof(dev->last_report), &dev->last_len, &dev->read_ovl)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return NRF_OCD_ERR_IO;
            dev->read_pending = true;
        }
    }
    DWORD wait = WaitForSingleObject(dev->read_ovl.hEvent, (DWORD)(timeout_ms < 0 ? INFINITE : timeout_ms));
    if (wait == WAIT_TIMEOUT) {
        CancelIo(dev->handle);
        GetOverlappedResult(dev->handle, &dev->read_ovl, &dev->last_len, TRUE);
        dev->read_pending = false;
        return NRF_OCD_ERR_TIMEOUT;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(dev->handle, &dev->read_ovl, &transferred, FALSE)) {
        dev->read_pending = false;
        return NRF_OCD_ERR_IO;
    }
    dev->last_len = transferred;
    dev->read_pending = false;
    ResetEvent(dev->read_ovl.hEvent);
    size_t n = dev->last_len;
    if (n > buf_size) n = buf_size;
    memcpy(buf, dev->last_report, n);
    if (out_len) *out_len = n;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    if (!dev || !data) return NRF_OCD_ERR_INVALID_ARG;
    (void)timeout_ms;
    DWORD written = 0;
    OVERLAPPED ovl = {0};
    ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(dev->handle, data, (DWORD)len, &written, &ovl)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(ovl.hEvent);
            return NRF_OCD_ERR_IO;
        }
        if (WaitForSingleObject(ovl.hEvent, (DWORD)(timeout_ms < 0 ? INFINITE : timeout_ms)) != WAIT_OBJECT_0) {
            CancelIo(dev->handle);
            CloseHandle(ovl.hEvent);
            return NRF_OCD_ERR_TIMEOUT;
        }
        GetOverlappedResult(dev->handle, &ovl, &written, FALSE);
    }
    CloseHandle(ovl.hEvent);
    if (written == 0) return NRF_OCD_ERR_IO;
    return NRF_OCD_OK;
}

const char *hid_path(const hid_device_t *dev)     { return dev ? dev->path : ""; }
const char *hid_serial(const hid_device_t *dev)   { return dev ? dev->serial : ""; }
uint16_t    hid_vid(const hid_device_t *dev)      { return dev ? dev->vid : 0; }
uint16_t    hid_pid(const hid_device_t *dev)      { return dev ? dev->pid : 0; }
int         hid_report_size(const hid_device_t *dev) { return dev ? dev->report_size : 64; }

bool hid_is_bulk(const hid_device_t *dev) { (void)dev; return false; }
void hid_mark_bulk(hid_device_t *dev) { (void)dev; }
