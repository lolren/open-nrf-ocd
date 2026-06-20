/* probe.h - USB / HID probe enumeration policy. */
#ifndef NRF_OCD_PROBE_H
#define NRF_OCD_PROBE_H

#include "hid.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char     serial[64];
    char     product[128];
    char     manufacturer[128];
    char     path[512];
} probe_info_t;

/* Fill out with a list of CMSIS-DAP probes. out_count is set on return. */
nrf_ocd_status_t probe_list(probe_info_t *out, size_t max_count, size_t *out_count);

/* Open a probe by serial (exact match, case-insensitive) or by index.
 * Returns the hid_device ready for CMSIS-DAP use. */
nrf_ocd_status_t probe_open(probe_info_t *out_info, hid_device_t **out_dev,
                            const char *serial, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_PROBE_H */
