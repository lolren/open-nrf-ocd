/* util.h - small helpers (endian, time, hex print). */
#ifndef NRF_OCD_UTIL_H
#define NRF_OCD_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Bit manipulation ----------------------------------------------------- */
static inline uint16_t read_le16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
static inline uint32_t read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return  (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}
static inline uint64_t read_le64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return  (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}
static inline void write_le16(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
}
static inline void write_le32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}
static inline void write_le64(void *p, uint64_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    b[4] = (uint8_t)(v >> 32);
    b[5] = (uint8_t)(v >> 40);
    b[6] = (uint8_t)(v >> 48);
    b[7] = (uint8_t)(v >> 56);
}
static inline uint32_t read_be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}
static inline void write_be32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)(v & 0xFF);
}

/* ----- Time ----------------------------------------------------------------- */
void nrf_ocd_sleep_ms(uint32_t ms);
uint64_t nrf_ocd_monotonic_ms(void);

/* ----- String helpers ------------------------------------------------------- */
char *nrf_ocd_strdup(const char *s);
char *nrf_ocd_strndup(const char *s, size_t n);
int   nrf_ocd_strcasecmp(const char *a, const char *b);
int   nrf_ocd_strncasecmp(const char *a, const char *b, size_t n);
bool  nrf_ocd_str_endswith(const char *s, const char *suffix);
bool  nrf_ocd_str_startswith(const char *s, const char *prefix);
void  nrf_ocd_str_rstrip(char *s);

/* ----- Hex/byte helpers ---------------------------------------------------- */
void nrf_ocd_hex_dump(const void *data, size_t size, size_t base_addr, FILE *out);

/* ----- Bit-reverse (used by SWD) ------------------------------------------- */
uint8_t nrf_ocd_rev8(uint8_t v);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_UTIL_H */
