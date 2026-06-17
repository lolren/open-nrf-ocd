/* hex.c - Intel HEX parser.
 *
 * Supports extended segment address (02) and extended linear address (04).
 * Ignores EOF (01) and start segment address (03/05) records.
 */
#include "hex.h"
#include "log.h"
#include "nrf_ocd.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_byte(const char *p) {
    int hi = (p[0] >= '0' && p[0] <= '9') ? p[0] - '0'
           : (p[0] >= 'A' && p[0] <= 'F') ? p[0] - 'A' + 10
           : (p[0] >= 'a' && p[0] <= 'f') ? p[0] - 'a' + 10
           : -1;
    int lo = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0'
           : (p[1] >= 'A' && p[1] <= 'F') ? p[1] - 'A' + 10
           : (p[1] >= 'a' && p[1] <= 'f') ? p[1] - 'a' + 10
           : -1;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

nrf_ocd_status_t hex_image_init(hex_image_t *img) {
    if (!img) return NRF_OCD_ERR_INVALID_ARG;
    memset(img, 0, sizeof(*img));
    return NRF_OCD_OK;
}

void hex_image_free(hex_image_t *img) {
    if (!img) return;
    for (size_t i = 0; i < img->count; i++) {
        free(img->segments[i].data);
    }
    free(img->segments);
    memset(img, 0, sizeof(*img));
}

static nrf_ocd_status_t append_segment(hex_image_t *img, uint32_t addr,
                                       const uint8_t *data, size_t n) {
    if (n == 0) return NRF_OCD_OK;
    /* Coalesce with previous segment if adjacent. */
    if (img->count > 0) {
        hex_segment_t *prev = &img->segments[img->count - 1];
        if (prev->address + prev->size == addr) {
            uint8_t *grown = (uint8_t *)realloc(prev->data, prev->size + n);
            if (!grown) return NRF_OCD_ERR_NO_MEM;
            memcpy(grown + prev->size, data, n);
            prev->data = grown;
            prev->size += n;
            return NRF_OCD_OK;
        }
    }
    if (img->count >= img->capacity) {
        size_t newcap = img->capacity ? img->capacity * 2 : 8;
        hex_segment_t *p = (hex_segment_t *)realloc(img->segments, newcap * sizeof(*p));
        if (!p) return NRF_OCD_ERR_NO_MEM;
        img->segments = p;
        img->capacity = newcap;
    }
    hex_segment_t *seg = &img->segments[img->count++];
    seg->address = addr;
    seg->size = n;
    seg->data = (uint8_t *)malloc(n);
    if (!seg->data) {
        img->count--;
        return NRF_OCD_ERR_NO_MEM;
    }
    memcpy(seg->data, data, n);
    return NRF_OCD_OK;
}

nrf_ocd_status_t hex_image_load_buffer(hex_image_t *img, const char *buf, size_t len) {
    if (!img || !buf) return NRF_OCD_ERR_INVALID_ARG;
    uint32_t base_addr = 0;
    size_t i = 0;
    int line_no = 0;
    while (i < len) {
        line_no++;
        /* Find end of line. */
        size_t eol = i;
        while (eol < len && buf[eol] != '\n' && buf[eol] != '\r') eol++;
        if (eol == i) { i = eol + 1; continue; }
        if (buf[i] != ':') {
            LOG_ERROR("HEX line %d: missing ':' prefix", line_no);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        size_t llen = eol - i - 1;
        if (llen < 10) {
            LOG_ERROR("HEX line %d: too short", line_no);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        int byte_count = hex_byte(buf + i + 1);
        int addr_lo    = (hex_byte(buf + i + 3) << 8) | hex_byte(buf + i + 5);
        int rec_type   = hex_byte(buf + i + 7);
        if (byte_count < 0 || rec_type < 0) {
            LOG_ERROR("HEX line %d: bad hex digits", line_no);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        /* Check the data length: 1 byte count + 2 addr + 1 type + N data + 1 checksum. */
        if (llen != (size_t)(1 + 2 + 1 + byte_count + 1) * 2) {
            LOG_ERROR("HEX line %d: length mismatch (got %zu, expected %zu)",
                      line_no, llen, (size_t)(1 + 2 + 1 + byte_count + 1) * 2);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        /* Parse data + verify checksum. */
        uint8_t data[256];
        if ((int)sizeof(data) < byte_count) {
            LOG_ERROR("HEX line %d: record too large (%d bytes)", line_no, byte_count);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        uint8_t cksum = (uint8_t)byte_count;
        cksum += (uint8_t)(addr_lo & 0xFF);
        cksum += (uint8_t)((addr_lo >> 8) & 0xFF);
        cksum += (uint8_t)rec_type;
        for (int b = 0; b < byte_count; b++) {
            data[b] = (uint8_t)hex_byte(buf + i + 9 + b * 2);
            cksum += data[b];
        }
        int cksum_file = hex_byte(buf + i + 9 + byte_count * 2);
        cksum = (uint8_t)((-cksum) & 0xFF);
        if (cksum != cksum_file) {
            LOG_ERROR("HEX line %d: checksum mismatch (got %02x, expected %02x)",
                      line_no, cksum_file, cksum);
            return NRF_OCD_ERR_CRC_MISMATCH;
        }

        switch (rec_type) {
            case 0x00: {
                /* Data record. */
                uint32_t addr = base_addr | (uint32_t)addr_lo;
                nrf_ocd_status_t st = append_segment(img, addr, data, (size_t)byte_count);
                if (st != NRF_OCD_OK) return st;
                break;
            }
            case 0x01:
                /* EOF. */
                return NRF_OCD_OK;
            case 0x02: {
                /* Extended segment address (left shift 4). */
                if (byte_count != 2) {
                    LOG_ERROR("HEX line %d: bad ELA record", line_no);
                    return NRF_OCD_ERR_FILE_FORMAT;
                }
                base_addr = ((uint32_t)data[0] << 8) | data[1];
                base_addr <<= 4;
                break;
            }
            case 0x03:
            case 0x05:
                /* Start address records - ignore. */
                break;
            case 0x04: {
                /* Extended linear address (left shift 16). */
                if (byte_count != 2) {
                    LOG_ERROR("HEX line %d: bad ELA record", line_no);
                    return NRF_OCD_ERR_FILE_FORMAT;
                }
                base_addr = ((uint32_t)data[0] << 8) | data[1];
                base_addr <<= 16;
                break;
            }
            default:
                LOG_ERROR("HEX line %d: unknown record type 0x%02x", line_no, rec_type);
                return NRF_OCD_ERR_FILE_FORMAT;
        }
        i = eol;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    return NRF_OCD_OK;
}

nrf_ocd_status_t hex_image_load(hex_image_t *img, const char *path) {
    if (!img || !path) return NRF_OCD_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("Could not open %s: %s", path, strerror(errno));
        return NRF_OCD_ERR_FILE_OPEN;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return NRF_OCD_ERR_IO; }
    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return NRF_OCD_ERR_NO_MEM; }
    size_t n = fread(buf, 1, (size_t)fsize, f);
    buf[n] = '\0';
    fclose(f);
    nrf_ocd_status_t st = hex_image_load_buffer(img, buf, n);
    free(buf);
    return st;
}

nrf_ocd_status_t hex_image_total_size(const hex_image_t *img, size_t *out) {
    if (!img || !out) return NRF_OCD_ERR_INVALID_ARG;
    size_t total = 0;
    for (size_t i = 0; i < img->count; i++) {
        total += img->segments[i].size;
    }
    *out = total;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hex_image_load_buffer_segment(hex_image_t *img, uint32_t addr,
                                               const uint8_t *data, size_t n) {
    if (!img || (!data && n > 0)) return NRF_OCD_ERR_INVALID_ARG;
    return append_segment(img, addr, data, n);
}
