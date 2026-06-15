/*
 * intelhex.c - Intel HEX file parser
 *
 * Parses .hex files into memory segments for flash programming.
 * Supports record types:
 *   00 - Data record
 *   01 - End of file
 *   02 - Extended segment address
 *   04 - Extended linear address
 */

#include "nrf_ocd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_byte(const char *s) {
    int hi = s[0] >= '0' && s[0] <= '9' ? (s[0] - '0') :
             s[0] >= 'A' && s[0] <= 'F' ? (s[0] - 'A' + 10) :
             s[0] >= 'a' && s[0] <= 'f' ? (s[0] - 'a' + 10) : -1;
    int lo = s[1] >= '0' && s[1] <= '9' ? (s[1] - '0') :
             s[1] >= 'A' && s[1] <= 'F' ? (s[1] - 'A' + 10) :
             s[1] >= 'a' && s[1] <= 'f' ? (s[1] - 'a' + 10) : -1;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

static nrf_ocd_error_t hex_parse_line(const char *line, uint8_t *type, uint32_t *addr,
                                       uint8_t **data, int *data_len) {
    /* Intel HEX format: :LLAAAATT[DD...]CC */

    if (line[0] != ':')
        return NRF_OCD_ERR_HEX_PARSE;

    int byte_count = hex_byte(line + 1);
    if (byte_count < 0)
        return NRF_OCD_ERR_HEX_PARSE;

    uint32_t address = (uint32_t)((hex_byte(line + 3) << 8) | hex_byte(line + 5));

    int record_type = hex_byte(line + 7);
    if (record_type < 0)
        return NRF_OCD_ERR_HEX_PARSE;

    /* Parse data bytes */
    uint8_t *data_buf = malloc(byte_count > 0 ? (size_t)byte_count : 1);
    if (!data_buf)
        return NRF_OCD_ERR_MEMORY;

    for (int i = 0; i < byte_count; i++) {
        int b = hex_byte(line + 9 + i * 2);
        if (b < 0) {
            free(data_buf);
            return NRF_OCD_ERR_HEX_PARSE;
        }
        data_buf[i] = (uint8_t)b;
    }

    /* Verify checksum */
    int sum = byte_count + ((address >> 8) & 0xFF) + (address & 0xFF) + record_type;
    for (int i = 0; i < byte_count; i++)
        sum += data_buf[i];
    sum = (-sum) & 0xFF;

    int checksum = hex_byte(line + 9 + byte_count * 2);
    if (checksum < 0) {
        free(data_buf);
        return NRF_OCD_ERR_HEX_PARSE;
    }
    if (sum != checksum) {
        NRF_WARN("Checksum mismatch at address 0x%08X (expected 0x%02X, got 0x%02X)",
                 address, sum, checksum);
    }

    *type = (uint8_t)record_type;
    *addr = address;
    *data = data_buf;
    *data_len = byte_count;

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_hex_parse(const char *filename, nrf_hex_file_t *out) {
    nrf_ocd_error_t result = NRF_OCD_OK;
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        NRF_ERR("Cannot open file: %s", filename);
        return NRF_OCD_ERR_HEX_PARSE;
    }

    memset(out, 0, sizeof(*out));
    out->capacity = 16;
    out->segments = calloc((size_t)out->capacity, sizeof(nrf_hex_segment_t));
    if (!out->segments) {
        fclose(fp);
        return NRF_OCD_ERR_MEMORY;
    }

    char line[256];
    uint32_t linear_base = 0;

    while (fgets(line, sizeof(line), fp)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (len > 1 && line[len - 2] == '\r')
            line[len - 2] = '\0';

        if (line[0] != ':')
            continue;

        uint8_t type;
        uint32_t addr;
        uint8_t *data;
        int data_len;

        nrf_ocd_error_t line_err = hex_parse_line(line, &type, &addr, &data, &data_len);
        if (line_err != NRF_OCD_OK) {
            if (data) free(data);
            continue;
        }

        switch (type) {
            case 0x00: /* Data record */
                addr += linear_base;

                if (out->count > 0) {
                    nrf_hex_segment_t *last = &out->segments[out->count - 1];
                    uint32_t last_end = last->addr + (uint32_t)last->len;
                    if (addr == last_end) {
                        uint8_t *new_data = realloc(last->data,
                            (size_t)(last->len + data_len));
                        if (new_data) {
                            memcpy(new_data + last->len, data, (size_t)data_len);
                            last->data = new_data;
                            last->len += data_len;
                            free(data);
                            continue;
                        }
                    }
                }

                if (out->count >= out->capacity) {
                    out->capacity *= 2;
                    nrf_hex_segment_t *new_segs = realloc(out->segments,
                        (size_t)out->capacity * sizeof(nrf_hex_segment_t));
                    if (!new_segs) {
                        free(data);
                        result = NRF_OCD_ERR_MEMORY;
                        goto done;
                    }
                    out->segments = new_segs;
                }

                out->segments[out->count].addr = addr;
                out->segments[out->count].data = data;
                out->segments[out->count].len = data_len;
                out->count++;
                break;

            case 0x01: /* End of file */
                free(data);
                result = NRF_OCD_OK;
                goto done;

            case 0x02: /* Extended segment address */
                if (data_len >= 2) {
                    linear_base = ((uint32_t)data[0] << 8 | (uint32_t)data[1]) << 4;
                }
                free(data);
                break;

            case 0x04: /* Extended linear address */
                if (data_len >= 2) {
                    linear_base = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16;
                }
                free(data);
                break;

            default:
                NRF_WARN("Unknown HEX record type 0x%02X", type);
                free(data);
                break;
        }
    }

    result = NRF_OCD_OK;

done:
    fclose(fp);
    return result;
}

void nrf_hex_free(nrf_hex_file_t *hex) {
    if (!hex)
        return;

    for (int i = 0; i < hex->count; i++) {
        free(hex->segments[i].data);
    }
    free(hex->segments);
    memset(hex, 0, sizeof(*hex));
}
