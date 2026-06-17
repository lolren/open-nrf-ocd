/* hex.h - Intel HEX parser. */
#ifndef NRF_OCD_HEX_H
#define NRF_OCD_HEX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single contiguous block of bytes loaded from a HEX file. The HEX
 * file may not be sorted: we keep segments in load order and let the
 * caller coalesce if needed. */
typedef struct {
    uint32_t address;
    uint8_t *data;
    size_t   size;     /* bytes */
} hex_segment_t;

typedef struct {
    hex_segment_t *segments;
    size_t         count;
    size_t         capacity;
} hex_image_t;

nrf_ocd_status_t hex_image_init(hex_image_t *img);
void             hex_image_free(hex_image_t *img);
nrf_ocd_status_t hex_image_load(hex_image_t *img, const char *path);
nrf_ocd_status_t hex_image_load_buffer(hex_image_t *img, const char *buf, size_t len);

/* Append a contiguous run of bytes. Coalesces with the previous segment
 * if the address is adjacent. */
nrf_ocd_status_t hex_image_load_buffer_segment(hex_image_t *img, uint32_t addr,
                                               const uint8_t *data, size_t n);

/* Concatenate adjacent segments and return a single block at a
 * particular address. Used by load commands. */
nrf_ocd_status_t hex_image_total_size(const hex_image_t *img, size_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_HEX_H */
