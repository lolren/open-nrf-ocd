/* nrf_ocd.h - common types and constants for nrf_ocd
 *
 * Single header that pulls in the platform abstraction plus the shared
 * error / logging / utility surface. Every translation unit that lives
 * in src/ starts by including this file.
 */
#ifndef NRF_OCD_H
#define NRF_OCD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Library version. Bumped when behaviour changes. */
#ifndef NRF_OCD_VERSION
#define NRF_OCD_VERSION "0.3.2"
#endif

/* Common status codes used throughout the codebase. */
typedef enum {
    NRF_OCD_OK = 0,
    NRF_OCD_ERR_GENERIC = -1,
    NRF_OCD_ERR_INVALID_ARG = -2,
    NRF_OCD_ERR_IO = -3,
    NRF_OCD_ERR_TIMEOUT = -4,
    NRF_OCD_ERR_PROBE_NOT_FOUND = -5,
    NRF_OCD_ERR_PROBE_OPEN = -6,
    NRF_OCD_ERR_PROBE_IO = -7,
    NRF_OCD_ERR_PROTOCOL = -8,
    NRF_OCD_ERR_FAULT = -9,
    NRF_OCD_ERR_LOCKED = -10,
    NRF_OCD_ERR_UNSUPPORTED = -11,
    NRF_OCD_ERR_FILE_OPEN = -12,
    NRF_OCD_ERR_FILE_FORMAT = -13,
    NRF_OCD_ERR_NO_MEM = -14,
    NRF_OCD_ERR_CRC_MISMATCH = -15,
    NRF_OCD_ERR_FLASH_INIT = -16,
    NRF_OCD_ERR_FLASH_ERASE = -17,
    NRF_OCD_ERR_FLASH_PROGRAM = -18,
} nrf_ocd_status_t;

const char *nrf_ocd_strerror(nrf_ocd_status_t s);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_H */
