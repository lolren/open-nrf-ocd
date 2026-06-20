/* hid_windows.c - Windows CMSIS-DAP transport.
 *
 * Prefer CMSIS-DAP v2 over WinUSB when the probe exposes it. This avoids the
 * broken HID DAP_Connect/DAP_Transfer behaviour seen on Seeed XIAO nRF54
 * probes while keeping a HID fallback for older CMSIS-DAP v1 adapters.
 */
#include "hid.h"
#include "log.h"
#include "util.h"

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <winusb.h>
#include <usb.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Microsoft HID interface GUID. */
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

/* CMSIS-DAP v2 WCID interface GUID used by DAPLink-class probes. */
static const GUID GUID_DEVINTERFACE_CMSIS_DAP_V2 =
    { 0xCDB3B5AD, 0x293B, 0x4663, { 0xAA, 0x36, 0x1A, 0xAE, 0x46, 0x46, 0x37, 0x76 } };

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "cfgmgr32.lib")

#define WINUSB_PREFIX "winusb:"
#define DAP_BULK_PACKET_SIZE 512U

struct hid_device {
    HANDLE   handle;
    WINUSB_INTERFACE_HANDLE usb;
    bool     is_bulk;
    UCHAR    in_endpoint;
    UCHAR    out_endpoint;
    ULONG    out_max_packet;
    ULONG    packet_size;

    OVERLAPPED read_ovl;
    bool     read_pending;
    uint8_t  last_report[NRF_OCD_HID_REPORT_SIZE];
    DWORD    last_len;

    char     path[512];
    char     serial[64];
    uint16_t vid;
    uint16_t pid;
    int      report_size;
    int      input_report_size;
    int      output_report_size;
};

struct hid_enumerate_handle {
    int      index;
    bool     built;
    int      capacity;
    hid_device_info_t *items;
    int      count;
    hid_device_info_t current;
};

static void wstr_to_str(const wchar_t *w, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = '\0';
}

static void counted_wstr_to_str(const wchar_t *w, int wchars, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!w || wchars <= 0) return;
    WideCharToMultiByte(CP_UTF8, 0, w, wchars, out, (int)cap, NULL, NULL);
    out[cap - 1] = '\0';
}

static bool str_has_prefix(const char *s, const char *prefix) {
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool ensure_capacity(hid_enumerate_handle_t *h) {
    if (h->count < h->capacity) return true;
    int new_capacity = h->capacity ? h->capacity * 2 : 16;
    hid_device_info_t *p = (hid_device_info_t *)realloc(
        h->items, (size_t)new_capacity * sizeof(*p));
    if (!p) return false;
    h->items = p;
    h->capacity = new_capacity;
    return true;
}

static bool has_bulk_duplicate(const hid_enumerate_handle_t *h,
                               uint16_t vid, uint16_t pid,
                               const char *serial) {
    if (!h || !serial || !serial[0]) return false;
    for (int i = 0; i < h->count; i++) {
        const hid_device_info_t *item = &h->items[i];
        if (!item->is_dap_v2) continue;
        if (item->vendor_id == vid && item->product_id == pid &&
            nrf_ocd_strcasecmp(item->serial_number, serial) == 0) {
            return true;
        }
    }
    return false;
}

static bool append_info(hid_enumerate_handle_t *h, const hid_device_info_t *info) {
    if (!ensure_capacity(h)) return false;
    h->items[h->count++] = *info;
    return true;
}

static bool get_string_descriptor(WINUSB_INTERFACE_HANDLE usb, UCHAR index,
                                  char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (index == 0) return false;

    UCHAR raw[256];
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(usb, USB_STRING_DESCRIPTOR_TYPE, index, 0x0409,
                              raw, sizeof(raw), &got)) {
        if (!WinUsb_GetDescriptor(usb, USB_STRING_DESCRIPTOR_TYPE, index, 0,
                                  raw, sizeof(raw), &got)) {
            return false;
        }
    }
    if (got < 4 || raw[1] != USB_STRING_DESCRIPTOR_TYPE) return false;
    int bytes = raw[0] >= 2 ? raw[0] - 2 : 0;
    if (bytes <= 0 || (ULONG)(bytes + 2) > got) return false;
    counted_wstr_to_str((const wchar_t *)(const void *)(raw + 2),
                        bytes / (int)sizeof(wchar_t), out, cap);
    return out[0] != '\0';
}

static void serial_from_parent_devinst(DEVINST devinst, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (out[0]) return;

    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, devinst, 0) != CR_SUCCESS) return;

    char id[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDA(parent, id, sizeof(id), 0) != CR_SUCCESS) return;

    const char *serial = strrchr(id, '\\');
    if (!serial || !serial[1]) return;
    serial++;
    size_t len = strlen(serial);
    if (len == 0 || len >= cap) return;
    memcpy(out, serial, len + 1);
}

static void setupdi_string_property(HDEVINFO dev_info, SP_DEVINFO_DATA *did,
                                    DWORD prop, char *out, size_t cap) {
    if (!out || cap == 0 || out[0]) return;
    DWORD type = 0;
    BYTE buf[256];
    if (!SetupDiGetDeviceRegistryPropertyA(dev_info, did, prop, &type,
                                           buf, sizeof(buf), NULL)) {
        return;
    }
    if (type != REG_SZ) return;
    strncpy(out, (const char *)buf, cap - 1);
    out[cap - 1] = '\0';
}

static bool fill_info_from_winusb_path(const char *path, SP_DEVINFO_DATA *did,
                                       hid_device_info_t *info) {
    memset(info, 0, sizeof(*info));

    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            NULL);
    if (fh == INVALID_HANDLE_VALUE) return false;

    WINUSB_INTERFACE_HANDLE usb = NULL;
    if (!WinUsb_Initialize(fh, &usb)) {
        CloseHandle(fh);
        return false;
    }

    USB_DEVICE_DESCRIPTOR desc;
    ULONG got = 0;
    if (WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             (PUCHAR)&desc, sizeof(desc), &got) &&
        got >= sizeof(desc)) {
        info->vendor_id = desc.idVendor;
        info->product_id = desc.idProduct;
        (void)get_string_descriptor(usb, desc.iManufacturer,
                                    info->manufacturer, sizeof(info->manufacturer));
        (void)get_string_descriptor(usb, desc.iProduct,
                                    info->product_string, sizeof(info->product_string));
        (void)get_string_descriptor(usb, desc.iSerialNumber,
                                    info->serial_number, sizeof(info->serial_number));
    }

    if (!info->serial_number[0] && did) {
        serial_from_parent_devinst(did->DevInst, info->serial_number,
                                   sizeof(info->serial_number));
    }
    if (!info->product_string[0]) {
        strncpy(info->product_string, "CMSIS-DAP v2 Adapter",
                sizeof(info->product_string) - 1);
    }
    snprintf(info->path, sizeof(info->path), "%s%s", WINUSB_PREFIX, path);
    info->is_dap_v2 = true;

    WinUsb_Free(usb);
    CloseHandle(fh);
    return info->vendor_id != 0 && info->product_id != 0;
}

static void enumerate_winusb(hid_enumerate_handle_t *h) {
    HDEVINFO dev_info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_CMSIS_DAP_V2,
                                             NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL,
                                                  &GUID_DEVINTERFACE_CMSIS_DAP_V2,
                                                  i, &if_data); i++) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, NULL, 0, &required, NULL);
        if (required == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(required);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);

        SP_DEVINFO_DATA did;
        memset(&did, 0, sizeof(did));
        did.cbSize = sizeof(did);
        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, detail,
                                              required, NULL, &did)) {
            free(detail);
            continue;
        }

        hid_device_info_t info;
        if (fill_info_from_winusb_path(detail->DevicePath, &did, &info)) {
            setupdi_string_property(dev_info, &did, SPDRP_FRIENDLYNAME,
                                    info.product_string, sizeof(info.product_string));
            setupdi_string_property(dev_info, &did, SPDRP_DEVICEDESC,
                                    info.product_string, sizeof(info.product_string));
            (void)append_info(h, &info);
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
}

static void enumerate_hid(hid_enumerate_handle_t *h) {
    HDEVINFO dev_info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_HID_LOCAL,
                                             NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA dev_data;
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL,
                                                  &GUID_DEVINTERFACE_HID_LOCAL,
                                                  i, &dev_data); i++) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &dev_data, NULL, 0, &required, NULL);
        if (required == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(required);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);

        SP_DEVINFO_DATA did;
        memset(&did, 0, sizeof(did));
        did.cbSize = sizeof(did);
        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &dev_data, detail,
                                              required, NULL, &did)) {
            free(detail);
            continue;
        }

        HANDLE h2 = CreateFileA(detail->DevicePath, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (h2 == INVALID_HANDLE_VALUE) {
            free(detail);
            continue;
        }

        HIDD_ATTRIBUTES attr;
        memset(&attr, 0, sizeof(attr));
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(h2, &attr)) {
            CloseHandle(h2);
            free(detail);
            continue;
        }

        hid_device_info_t info;
        memset(&info, 0, sizeof(info));
        info.vendor_id  = attr.VendorID;
        info.product_id = attr.ProductID;
        strncpy(info.path, detail->DevicePath, sizeof(info.path) - 1);

        wchar_t buf[256];
        if (HidD_GetSerialNumberString(h2, buf, sizeof(buf)))
            wstr_to_str(buf, info.serial_number, sizeof(info.serial_number));
        if (HidD_GetProductString(h2, buf, sizeof(buf)))
            wstr_to_str(buf, info.product_string, sizeof(info.product_string));
        if (HidD_GetManufacturerString(h2, buf, sizeof(buf)))
            wstr_to_str(buf, info.manufacturer, sizeof(info.manufacturer));

        CloseHandle(h2);
        free(detail);

        if (has_bulk_duplicate(h, info.vendor_id, info.product_id,
                               info.serial_number)) {
            continue;
        }
        (void)append_info(h, &info);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
}

static void build_items(hid_enumerate_handle_t *h) {
    if (!h || h->built) return;
    enumerate_winusb(h);
    enumerate_hid(h);
    h->built = true;
}

nrf_ocd_status_t hid_init(void) { return NRF_OCD_OK; }
void             hid_shutdown(void) {}

hid_enumerate_handle_t *hid_enumerate_start(void) {
    return (hid_enumerate_handle_t *)calloc(1, sizeof(hid_enumerate_handle_t));
}

const hid_device_info_t *hid_enumerate_next(hid_enumerate_handle_t *h) {
    if (!h) return NULL;
    build_items(h);
    if (h->index >= h->count) return NULL;
    h->current = h->items[h->index++];
    return &h->current;
}

void hid_enumerate_free(hid_enumerate_handle_t *h) {
    if (!h) return;
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
            static __declspec(thread) hid_device_info_t copy;
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
            static __declspec(thread) hid_device_info_t copy;
            copy = *info;
            hid_enumerate_free(h);
            return &copy;
        }
        i++;
    }
    hid_enumerate_free(h);
    return NULL;
}

static void flush_winusb_in(hid_device_t *dev) {
    if (!dev || !dev->usb || !dev->in_endpoint) return;
    UCHAR tmp[DAP_BULK_PACKET_SIZE];
    ULONG timeout = 5;
    ULONG got = 0;
    (void)WinUsb_SetPipePolicy(dev->usb, dev->in_endpoint, PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout), &timeout);
    for (int i = 0; i < 32; i++) {
        if (!WinUsb_ReadPipe(dev->usb, dev->in_endpoint, tmp, sizeof(tmp),
                             &got, NULL) || got == 0) {
            break;
        }
    }
}

static hid_device_t *open_winusb_path(const char *prefixed_path) {
    const char *path = prefixed_path + strlen(WINUSB_PREFIX);
    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateFileA failed for %s: %lu", path, GetLastError());
        return NULL;
    }

    WINUSB_INTERFACE_HANDLE usb = NULL;
    if (!WinUsb_Initialize(fh, &usb)) {
        LOG_ERROR("WinUsb_Initialize failed for %s: %lu", path, GetLastError());
        CloseHandle(fh);
        return NULL;
    }

    USB_INTERFACE_DESCRIPTOR iface;
    if (!WinUsb_QueryInterfaceSettings(usb, 0, &iface)) {
        LOG_ERROR("WinUsb_QueryInterfaceSettings failed for %s: %lu", path, GetLastError());
        WinUsb_Free(usb);
        CloseHandle(fh);
        return NULL;
    }

    hid_device_t *dev = (hid_device_t *)calloc(1, sizeof(*dev));
    if (!dev) {
        WinUsb_Free(usb);
        CloseHandle(fh);
        return NULL;
    }
    dev->handle = fh;
    dev->usb = usb;
    dev->is_bulk = true;
    dev->packet_size = DAP_BULK_PACKET_SIZE;
    dev->report_size = (int)DAP_BULK_PACKET_SIZE;
    strncpy(dev->path, prefixed_path, sizeof(dev->path) - 1);

    for (UCHAR i = 0; i < iface.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe;
        if (!WinUsb_QueryPipe(usb, 0, i, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            if (!dev->in_endpoint) dev->in_endpoint = pipe.PipeId;
        } else {
            if (!dev->out_endpoint) {
                dev->out_endpoint = pipe.PipeId;
                dev->out_max_packet = pipe.MaximumPacketSize;
            }
        }
    }
    if (!dev->in_endpoint || !dev->out_endpoint) {
        LOG_ERROR("Could not find WinUSB bulk endpoints for %s", path);
        hid_close(dev);
        return NULL;
    }

    UCHAR zlp = TRUE;
    (void)WinUsb_SetPipePolicy(usb, dev->out_endpoint, SHORT_PACKET_TERMINATE,
                               sizeof(zlp), &zlp);

    USB_DEVICE_DESCRIPTOR desc;
    ULONG got = 0;
    if (WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             (PUCHAR)&desc, sizeof(desc), &got) &&
        got >= sizeof(desc)) {
        dev->vid = desc.idVendor;
        dev->pid = desc.idProduct;
        (void)get_string_descriptor(usb, desc.iSerialNumber,
                                    dev->serial, sizeof(dev->serial));
    }

    flush_winusb_in(dev);
    return dev;
}

static hid_device_t *open_hid_path(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateFileA failed for %s: %lu", path, GetLastError());
        return NULL;
    }
    hid_device_t *dev = (hid_device_t *)calloc(1, sizeof(*dev));
    if (!dev) { CloseHandle(h); return NULL; }
    dev->handle = h;
    strncpy(dev->path, path, sizeof(dev->path) - 1);

    HIDD_ATTRIBUTES attr;
    memset(&attr, 0, sizeof(attr));
    attr.Size = sizeof(attr);
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
            dev->input_report_size = caps.InputReportByteLength;
            dev->output_report_size = caps.OutputReportByteLength;
            if (dev->input_report_size > (int)sizeof(dev->last_report))
                dev->input_report_size = sizeof(dev->last_report);
            if (dev->output_report_size > (int)sizeof(dev->last_report))
                dev->output_report_size = sizeof(dev->last_report);
            dev->report_size = dev->output_report_size > 0
                                   ? dev->output_report_size
                                   : dev->input_report_size;
        }
        HidD_FreePreparsedData(ppd);
    }
    if (dev->input_report_size <= 0) dev->input_report_size = 65;
    if (dev->input_report_size > (int)sizeof(dev->last_report))
        dev->input_report_size = sizeof(dev->last_report);
    if (dev->output_report_size <= 0) dev->output_report_size = 65;
    if (dev->output_report_size > (int)sizeof(dev->last_report))
        dev->output_report_size = sizeof(dev->last_report);
    if (dev->report_size <= 0) dev->report_size = 64;
    if (dev->report_size > (int)sizeof(dev->last_report))
        dev->report_size = sizeof(dev->last_report);

    memset(&dev->read_ovl, 0, sizeof(dev->read_ovl));
    dev->read_ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    return dev;
}

hid_device_t *hid_open_path(const char *path) {
    if (!path) return NULL;
    if (str_has_prefix(path, WINUSB_PREFIX)) return open_winusb_path(path);
    return open_hid_path(path);
}

void hid_close(hid_device_t *dev) {
    if (!dev) return;
    if (dev->usb) WinUsb_Free(dev->usb);
    if (dev->read_ovl.hEvent) CloseHandle(dev->read_ovl.hEvent);
    if (dev->handle && dev->handle != INVALID_HANDLE_VALUE) CloseHandle(dev->handle);
    free(dev);
}

bool hid_has_report(hid_device_t *dev) {
    if (!dev || dev->is_bulk) return false;
    return dev->read_pending ? WaitForSingleObject(dev->read_ovl.hEvent, 0) == WAIT_OBJECT_0 : false;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *buf, size_t buf_size,
                          size_t *out_len, int timeout_ms) {
    if (!dev || !buf) return NRF_OCD_ERR_INVALID_ARG;
    if (dev->is_bulk) {
        ULONG timeout = (ULONG)(timeout_ms < 0 ? 0 : timeout_ms);
        (void)WinUsb_SetPipePolicy(dev->usb, dev->in_endpoint, PIPE_TRANSFER_TIMEOUT,
                                   sizeof(timeout), &timeout);
        ULONG transferred = 0;
        ULONG want = dev->packet_size ? dev->packet_size : DAP_BULK_PACKET_SIZE;
        if (want > buf_size) want = (ULONG)buf_size;
        if (!WinUsb_ReadPipe(dev->usb, dev->in_endpoint, buf, want,
                             &transferred, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT || err == ERROR_OPERATION_ABORTED) {
                return NRF_OCD_ERR_TIMEOUT;
            }
            LOG_DEBUG("WinUsb_ReadPipe failed: %lu", err);
            return NRF_OCD_ERR_IO;
        }
        if (out_len) *out_len = transferred;
        return NRF_OCD_OK;
    }

    if (!dev->read_pending) {
        const DWORD read_len = (DWORD)(dev->input_report_size > 0
                                           ? dev->input_report_size
                                           : (int)sizeof(dev->last_report));
        if (!ReadFile(dev->handle, dev->last_report, read_len, &dev->last_len, &dev->read_ovl)) {
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
    if (dev->is_bulk) {
        ULONG timeout = (ULONG)(timeout_ms < 0 ? 0 : timeout_ms);
        (void)WinUsb_SetPipePolicy(dev->usb, dev->out_endpoint, PIPE_TRANSFER_TIMEOUT,
                                   sizeof(timeout), &timeout);
        ULONG written = 0;
        if (!WinUsb_WritePipe(dev->usb, dev->out_endpoint, (PUCHAR)data,
                              (ULONG)len, &written, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT || err == ERROR_OPERATION_ABORTED) {
                return NRF_OCD_ERR_TIMEOUT;
            }
            LOG_DEBUG("WinUsb_WritePipe failed: %lu len=%zu", err, len);
            return NRF_OCD_ERR_IO;
        }
        return written == (ULONG)len ? NRF_OCD_OK : NRF_OCD_ERR_IO;
    }

    (void)timeout_ms;
    if (dev->output_report_size > 0 && len < (size_t)dev->output_report_size) {
        return NRF_OCD_ERR_INVALID_ARG;
    }
    DWORD written = 0;
    OVERLAPPED ovl;
    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(dev->handle, data, (DWORD)len, &written, &ovl)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            if (HidD_SetOutputReport(dev->handle, (PVOID)data, (ULONG)len)) {
                CloseHandle(ovl.hEvent);
                return NRF_OCD_OK;
            }
            CloseHandle(ovl.hEvent);
            return NRF_OCD_ERR_IO;
        }
        if (WaitForSingleObject(ovl.hEvent, (DWORD)(timeout_ms < 0 ? INFINITE : timeout_ms)) != WAIT_OBJECT_0) {
            CancelIo(dev->handle);
            CloseHandle(ovl.hEvent);
            return NRF_OCD_ERR_TIMEOUT;
        }
        if (!GetOverlappedResult(dev->handle, &ovl, &written, FALSE)) {
            CloseHandle(ovl.hEvent);
            return NRF_OCD_ERR_IO;
        }
    }
    CloseHandle(ovl.hEvent);
    if (written == 0) return NRF_OCD_ERR_IO;
    return NRF_OCD_OK;
}

const char *hid_path(const hid_device_t *dev)     { return dev ? dev->path : ""; }
const char *hid_serial(const hid_device_t *dev)   { return dev ? dev->serial : ""; }
uint16_t    hid_vid(const hid_device_t *dev)      { return dev ? dev->vid : 0; }
uint16_t    hid_pid(const hid_device_t *dev)      { return dev ? dev->pid : 0; }
int         hid_report_size(const hid_device_t *dev) {
    if (!dev) return 64;
    return dev->is_bulk ? (int)(dev->packet_size ? dev->packet_size : DAP_BULK_PACKET_SIZE)
                        : dev->report_size;
}

bool hid_is_bulk(const hid_device_t *dev) { return dev && dev->is_bulk; }
void hid_mark_bulk(hid_device_t *dev) { if (dev) dev->is_bulk = true; }
