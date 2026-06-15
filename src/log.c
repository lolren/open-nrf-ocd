/*
 * log.c - Logging facilities for nrf_ocd
 *
 * Thread-safe (lock-free) leveled logging. Separated from coresight.c
 * so that every module can log without dragging in DP/AP code.
 */

#include "nrf_ocd.h"
#include <stdio.h>
#include <stdarg.h>

static nrf_log_level_t g_log_level = NRF_LOG_INFO;

void nrf_log_set_level(nrf_log_level_t level) {
    g_log_level = level;
}

nrf_log_level_t nrf_log_get_level(void) {
    return g_log_level;
}

void nrf_log(nrf_log_level_t level, const char *fmt, ...) {
    if (level > g_log_level)
        return;

    const char *prefix;
    FILE *fp;

    switch (level) {
        case NRF_LOG_ERROR: prefix = "ERROR: "; fp = stderr; break;
        case NRF_LOG_WARN:  prefix = "WARN:  "; fp = stderr; break;
        case NRF_LOG_INFO:  prefix = "INFO:  "; fp = stderr; break;
        case NRF_LOG_DEBUG: prefix = "DEBUG: "; fp = stderr; break;
        default:            prefix = "";      fp = stderr; break;
    }

    fprintf(fp, "%s", prefix);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fprintf(fp, "\n");
    fflush(fp);
}

const char *nrf_ocd_error_str(nrf_ocd_error_t err) {
    switch (err) {
        case NRF_OCD_OK:                 return "OK";
        case NRF_OCD_ERR_NO_DEVICE:      return "No device found";
        case NRF_OCD_ERR_USB_OPEN:       return "USB open failed";
        case NRF_OCD_ERR_USB_WRITE:      return "USB write failed";
        case NRF_OCD_ERR_USB_READ:       return "USB read failed";
        case NRF_OCD_ERR_DAP_CMD:        return "CMSIS-DAP command error";
        case NRF_OCD_ERR_SWD_CONNECT:    return "SWD connect failed";
        case NRF_OCD_ERR_TRANSFER:       return "Transfer error";
        case NRF_OCD_ERR_TRANSFER_FAULT: return "Transfer fault";
        case NRF_OCD_ERR_TRANSFER_WAIT:  return "Transfer wait timeout";
        case NRF_OCD_ERR_FLASH_INIT:     return "Flash init failed";
        case NRF_OCD_ERR_FLASH_ERASE:    return "Flash erase failed";
        case NRF_OCD_ERR_FLASH_PROGRAM:  return "Flash program failed";
        case NRF_OCD_ERR_HEX_PARSE:      return "Intel HEX parse error";
        case NRF_OCD_ERR_MEMORY:         return "Memory allocation failed";
        case NRF_OCD_ERR_TIMEOUT:        return "Timeout";
        case NRF_OCD_ERR_TARGET_MISMATCH: return "Target mismatch";
        default:                         return "Unknown error";
    }
}
