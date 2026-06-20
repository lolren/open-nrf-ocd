/* util.c - implementation of small helpers. */
#define _POSIX_C_SOURCE 200809L

#include "util.h"
#ifdef _WIN32
#include "hid.h"
#endif
#include "nrf_ocd.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

void nrf_ocd_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {
        .tv_sec  = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000L),
    };
    nanosleep(&ts, NULL);
#endif
}

uint64_t nrf_ocd_monotonic_ms(void) {
#ifdef _WIN32
    /* GetTickCount64 is monotonic and millisecond resolution; sufficient for
     * command timeouts. */
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
#endif
}

char *nrf_ocd_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

char *nrf_ocd_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *r = (char *)malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

int nrf_ocd_strcasecmp(const char *a, const char *b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int nrf_ocd_strncasecmp(const char *a, const char *b, size_t n) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0' || cb == '\0') return (int)ca - (int)cb;
        int la = tolower(ca);
        int lb = tolower(cb);
        if (la != lb) return la - lb;
    }
    return 0;
}

bool nrf_ocd_str_endswith(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    return (ls >= lf) && (memcmp(s + ls - lf, suffix, lf) == 0);
}

bool nrf_ocd_str_startswith(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    size_t ls = strlen(s);
    size_t lp = strlen(prefix);
    return (ls >= lp) && (memcmp(s, prefix, lp) == 0);
}

void nrf_ocd_str_rstrip(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
}

void nrf_ocd_hex_dump(const void *data, size_t size, size_t base_addr, FILE *out) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i += 16) {
        fprintf(out, "%08zx  ", base_addr + i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) {
                fprintf(out, "%02x ", p[i + j]);
            } else {
                fprintf(out, "   ");
            }
            if (j == 7) fputc(' ', out);
        }
        fputc(' ', out);
        for (size_t j = 0; j < 16 && (i + j) < size; j++) {
            uint8_t c = p[i + j];
            fputc((c >= 0x20 && c < 0x7f) ? c : '.', out);
        }
        fputc('\n', out);
    }
}

uint8_t nrf_ocd_rev8(uint8_t v) {
    /* SWD line driver uses LSB-first parity; bit reverse is required for some
     * operations. Standard 0xEDB88320 reverse. */
    v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

const char *nrf_ocd_strerror(nrf_ocd_status_t s) {
    switch (s) {
        case NRF_OCD_OK:                  return "OK";
        case NRF_OCD_ERR_GENERIC:         return "Generic error";
        case NRF_OCD_ERR_INVALID_ARG:     return "Invalid argument";
        case NRF_OCD_ERR_IO:              return "I/O error";
        case NRF_OCD_ERR_TIMEOUT:         return "Timeout";
        case NRF_OCD_ERR_PROBE_NOT_FOUND: return "Probe not found";
        case NRF_OCD_ERR_PROBE_OPEN:      return "Probe open failed";
        case NRF_OCD_ERR_PROBE_IO:        return "Probe communication error";
        case NRF_OCD_ERR_PROTOCOL:        return "Protocol error";
        case NRF_OCD_ERR_FAULT:           return "Bus fault";
        case NRF_OCD_ERR_LOCKED:          return "Target is locked (APPROTECT)";
        case NRF_OCD_ERR_UNSUPPORTED:     return "Operation not supported";
        case NRF_OCD_ERR_FILE_OPEN:       return "File open failed";
        case NRF_OCD_ERR_FILE_FORMAT:     return "File format error";
        case NRF_OCD_ERR_NO_MEM:          return "Out of memory";
        case NRF_OCD_ERR_CRC_MISMATCH:    return "CRC mismatch";
        case NRF_OCD_ERR_FLASH_INIT:      return "Flash initialization failed";
        case NRF_OCD_ERR_FLASH_ERASE:     return "Flash erase failed";
        case NRF_OCD_ERR_FLASH_PROGRAM:   return "Flash programming failed";
    }
    return "Unknown";
}

#ifdef _WIN32
/* Microsoft COM-port device interface GUID. */
static const GUID GUID_DEVINTERFACE_COMPORT_LOCAL =
    { 0x86E0D1E0, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

static const char *find_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return NULL;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (nrf_ocd_strncasecmp(p, needle, needle_len) == 0) return p;
    }
    return NULL;
}

static void lowercase_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && (i + 1) < dst_size; i++) {
            dst[i] = (char)tolower((unsigned char)src[i]);
        }
    }
    dst[i] = '\0';
}

static int parse_windows_usb_vid_pid(const char *device_id,
                                     char *vid, size_t vid_size,
                                     char *pid, size_t pid_size) {
    if (!device_id || !vid || !pid || vid_size < 5 || pid_size < 5) return 0;
    const char *vid_pos = find_case_insensitive(device_id, "VID_");
    const char *pid_pos = find_case_insensitive(device_id, "PID_");
    if (!vid_pos || !pid_pos) return 0;
    vid_pos += 4;
    pid_pos += 4;
    for (size_t i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)vid_pos[i]) || !isxdigit((unsigned char)pid_pos[i])) {
            return 0;
        }
        vid[i] = (char)tolower((unsigned char)vid_pos[i]);
        pid[i] = (char)tolower((unsigned char)pid_pos[i]);
    }
    vid[4] = '\0';
    pid[4] = '\0';
    return 1;
}

static int extract_windows_usb_instance_stem(const char *device_id,
                                             char *stem, size_t stem_size) {
    if (!device_id || !stem || stem_size < 2) return 0;
    const char *instance = strrchr(device_id, '\\');
    if (!instance || !instance[1]) return 0;
    instance++;

    char tmp[128];
    lowercase_copy(tmp, sizeof(tmp), instance);
    nrf_ocd_str_rstrip(tmp);

    /* Composite COM ports normally end in "&0002" while the CMSIS-DAP HID
     * sibling ends in "&0000". Matching only the shared parent stem maps
     * COMx back to the correct HID probe serial.
     */
    char *last_amp = strrchr(tmp, '&');
    if (last_amp && strlen(last_amp + 1) == 4) {
        bool four_hex = true;
        for (size_t i = 0; i < 4; i++) {
            if (!isxdigit((unsigned char)last_amp[1 + i])) {
                four_hex = false;
                break;
            }
        }
        if (four_hex) *last_amp = '\0';
    }

    if (tmp[0] == '\0' || strlen(tmp) >= stem_size) return 0;
    strcpy(stem, tmp);
    return 1;
}

static int true_usb_serial_from_windows_device_id(const char *device_id,
                                                  char *buf, size_t buf_size) {
    if (!device_id || !buf || buf_size < 2) return 0;
    const char *serial = strrchr(device_id, '\\');
    if (!serial || !serial[1]) return 0;
    serial++;

    char tmp[128];
    lowercase_copy(tmp, sizeof(tmp), serial);
    nrf_ocd_str_rstrip(tmp);
    if (tmp[0] == '\0') return 0;

    /* A real DAPLink serial is a simple serial string. Composite interface
     * instance IDs contain '&' and are not usable as CMSIS-DAP serials.
     */
    if (strchr(tmp, '&')) return 0;

    size_t len = strlen(serial);
    while (len > 0 && (serial[len - 1] == '\n' || serial[len - 1] == '\r' ||
                       serial[len - 1] == ' ' || serial[len - 1] == '\t')) {
        len--;
    }
    if (len == 0 || len >= buf_size) return 0;
    memcpy(buf, serial, len);
    buf[len] = '\0';
    return 1;
}

static int hid_serial_from_windows_com_device_id(const char *device_id,
                                                 char *buf, size_t buf_size) {
    if (!device_id || !buf || buf_size < 2) return 0;

    char vid[5] = {0};
    char pid[5] = {0};
    char stem[128] = {0};
    if (!parse_windows_usb_vid_pid(device_id, vid, sizeof(vid), pid, sizeof(pid))) {
        return 0;
    }
    (void)extract_windows_usb_instance_stem(device_id, stem, sizeof(stem));

    char vid_pid_needle[32];
    snprintf(vid_pid_needle, sizeof(vid_pid_needle), "vid_%s&pid_%s", vid, pid);

    hid_enumerate_handle_t *h = hid_enumerate_start();
    if (!h) return 0;

    const hid_device_info_t *info;
    char single_vid_pid_serial[64] = {0};
    size_t vid_pid_matches = 0;
    while ((info = hid_enumerate_next(h)) != NULL) {
        char path_lower[512];
        lowercase_copy(path_lower, sizeof(path_lower), info->path);
        if (!strstr(path_lower, vid_pid_needle)) continue;

        vid_pid_matches++;
        if (single_vid_pid_serial[0] == '\0' && info->serial_number[0]) {
            strncpy(single_vid_pid_serial, info->serial_number,
                    sizeof(single_vid_pid_serial) - 1);
        }

        if (stem[0] && strstr(path_lower, stem) && info->serial_number[0]) {
            size_t len = strlen(info->serial_number);
            if (len > 0 && len < buf_size) {
                memcpy(buf, info->serial_number, len + 1);
                hid_enumerate_free(h);
                return 1;
            }
        }
    }

    if (vid_pid_matches == 1 && single_vid_pid_serial[0]) {
        size_t len = strlen(single_vid_pid_serial);
        if (len > 0 && len < buf_size) {
            memcpy(buf, single_vid_pid_serial, len + 1);
            hid_enumerate_free(h);
            return 1;
        }
    }

    hid_enumerate_free(h);
    return 0;
}

static const char *normalize_windows_port_name(const char *port) {
    if (!port) return NULL;
    const char *p = port;
    if ((p[0] == '\\' || p[0] == '/') &&
        (p[1] == '\\' || p[1] == '/') &&
        p[2] == '.' &&
        (p[3] == '\\' || p[3] == '/')) {
        p += 4;
    }
    const char *slash = strrchr(p, '\\');
    const char *fslash = strrchr(p, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    return slash ? slash + 1 : p;
}

static int port_device_id_from_setupapi(const char *port,
                                        char *device_id, size_t device_id_size) {
    if (!port || !device_id || device_id_size < 2) return 0;
    const char *want_port = normalize_windows_port_name(port);
    if (!want_port || !want_port[0]) return 0;

    HDEVINFO dev_info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_COMPORT_LOCAL,
                                             NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    int found = 0;
    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL,
                                                  &GUID_DEVINTERFACE_COMPORT_LOCAL,
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

        char port_name[64] = {0};
        HKEY key = SetupDiOpenDevRegKey(dev_info, &did, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD type = 0;
            DWORD len = sizeof(port_name);
            if (RegQueryValueExA(key, "PortName", NULL, &type,
                                 (LPBYTE)port_name, &len) != ERROR_SUCCESS ||
                type != REG_SZ) {
                port_name[0] = '\0';
            }
            RegCloseKey(key);
        }

        if (port_name[0] && nrf_ocd_strcasecmp(port_name, want_port) == 0) {
            if (SetupDiGetDeviceInstanceIdA(dev_info, &did, device_id,
                                            (DWORD)device_id_size, NULL)) {
                found = 1;
            }
            free(detail);
            break;
        }

        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}
#endif

int port_to_serial(const char *port, char *buf, size_t buf_size) {
    if (!port || !buf || buf_size < 2) return 0;
#ifdef _WIN32
    char device_id[512] = {0};
    if (!port_device_id_from_setupapi(port, device_id, sizeof(device_id))) {
        return 0;
    }
    if (!find_case_insensitive(device_id, "VID_") ||
        !find_case_insensitive(device_id, "PID_")) {
        return 0;
    }
    return hid_serial_from_windows_com_device_id(device_id, buf, buf_size) ||
           true_usb_serial_from_windows_device_id(device_id, buf, buf_size);
#else
    const char *tty = port;
    const char *last_slash = strrchr(port, '/');
    if (last_slash) tty = last_slash + 1;
    if (*tty == '\0') return 0;

    char device_path[512];
    int n = snprintf(device_path, sizeof(device_path), "/sys/class/tty/%s/device", tty);
    if (n < 0 || (size_t)n >= sizeof(device_path)) return 0;

    char current[PATH_MAX];
    if (!realpath(device_path, current)) return 0;

    FILE *f = NULL;
    for (unsigned i = 0; i < 8; i++) {
        const char *base = strrchr(current, '/');
        base = base ? base + 1 : current;
        if (nrf_ocd_str_startswith(base, "usb")) break;

        char serial_path[PATH_MAX];
        n = snprintf(serial_path, sizeof(serial_path), "%s/serial", current);
        if (n >= 0 && (size_t)n < sizeof(serial_path)) {
            f = fopen(serial_path, "r");
            if (f) break;
        }

        char *last = strrchr(current, '/');
        if (!last || last == current) break;
        *last = '\0';
    }

    if (!f) return 0;
    if (!fgets(buf, (int)buf_size, f)) { fclose(f); return 0; }
    fclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return (len > 0) ? 1 : 0;
#endif
}
