/* test_hex.c - unit tests for the Intel HEX parser. */
#include "hex.h"
#include "nrf_ocd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_basic(void) {
    hex_image_t img;
    hex_image_init(&img);
    const char *hex =
        ":020000040010EA\r\n"
        ":0401000023456788A4\r\n"
        ":00000001FF\r\n";
    nrf_ocd_status_t st = hex_image_load_buffer(&img, hex, strlen(hex));
    ASSERT(st == NRF_OCD_OK);
    ASSERT(img.count == 1);
    if (img.count >= 1) {
        ASSERT(img.segments[0].address == 0x00100100);
        ASSERT(img.segments[0].size == 4);
        ASSERT(img.segments[0].data[0] == 0x23);
        ASSERT(img.segments[0].data[1] == 0x45);
        ASSERT(img.segments[0].data[2] == 0x67);
        ASSERT(img.segments[0].data[3] == 0x88);
    }
    (void)0;  /* keep formatting happy */
    hex_image_free(&img);
    printf("test_basic: OK\n");
}

static void test_coalesce(void) {
    hex_image_t img;
    hex_image_init(&img);
    const char *hex =
        ":020000040000FA\r\n"
        ":020000001122CB\r\n"
        ":02000200334485\r\n"
        ":00000001FF\r\n";
    nrf_ocd_status_t st = hex_image_load_buffer(&img, hex, strlen(hex));
    ASSERT(st == NRF_OCD_OK);
    ASSERT(img.count == 1);
    if (img.count >= 1) {
        ASSERT(img.segments[0].address == 0x0000);
        ASSERT(img.segments[0].size == 4);
        ASSERT(img.segments[0].data[0] == 0x11);
        ASSERT(img.segments[0].data[3] == 0x44);
    }
    hex_image_free(&img);
    printf("test_coalesce: OK\n");
}

static void test_bad_checksum(void) {
    hex_image_t img;
    hex_image_init(&img);
    const char *hex = ":0401000023456788A5\r\n";
    nrf_ocd_status_t st = hex_image_load_buffer(&img, hex, strlen(hex));
    ASSERT(st == NRF_OCD_ERR_CRC_MISMATCH);
    hex_image_free(&img);
    printf("test_bad_checksum: OK\n");
}

static void test_total_size(void) {
    hex_image_t img;
    hex_image_init(&img);
    const char *hex =
        ":0400000000010203F6\r\n"
        ":0400040005060708DE\r\n"
        ":00000001FF\r\n";
    nrf_ocd_status_t st = hex_image_load_buffer(&img, hex, strlen(hex));
    ASSERT(st == NRF_OCD_OK);
    size_t total = 0;
    ASSERT(hex_image_total_size(&img, &total) == NRF_OCD_OK);
    ASSERT(total == 8);
    hex_image_free(&img);
    printf("test_total_size: OK\n");
}

int main(void) {
    test_basic();
    test_coalesce();
    test_bad_checksum();
    test_total_size();
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All HEX tests passed\n");
    return 0;
}
