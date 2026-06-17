/* util.c - implementation of small helpers. */
#define _POSIX_C_SOURCE 200809L

#include "util.h"
#include "nrf_ocd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
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
    }
    return "Unknown";
}
