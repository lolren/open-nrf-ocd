/* test_cmsis_dap_close.c - post-reset CMSIS-DAP teardown tests. */
#include "cmsis_dap.h"
#include "hid.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct hid_device {
    int unused;
};

static int failures;
static int write_calls;
static int read_calls;
static int write_timeout;
static int read_timeout;
static size_t write_len;
static uint8_t write_data[NRF_OCD_HID_REPORT_SIZE];
static nrf_ocd_status_t write_status;
static nrf_ocd_status_t read_status;
static uint8_t read_data[NRF_OCD_HID_REPORT_SIZE];
static size_t read_len;
static int flush_calls;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    (void)dev;
    write_calls++;
    write_timeout = timeout_ms;
    write_len = len;
    if (len <= sizeof(write_data)) memcpy(write_data, data, len);
    return write_status;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *data, size_t capacity,
                          size_t *out_len, int timeout_ms) {
    (void)dev;
    read_calls++;
    read_timeout = timeout_ms;
    if (read_status != NRF_OCD_OK) return read_status;
    size_t count = read_len < capacity ? read_len : capacity;
    memcpy(data, read_data, count);
    *out_len = count;
    return NRF_OCD_OK;
}

void hid_flush(hid_device_t *dev) {
    (void)dev;
    flush_calls++;
}

static void reset_observation(void) {
    write_calls = 0;
    read_calls = 0;
    write_timeout = 0;
    read_timeout = 0;
    write_len = 0;
    memset(write_data, 0xA5, sizeof(write_data));
    write_status = NRF_OCD_OK;
    read_status = NRF_OCD_OK;
    memset(read_data, 0, sizeof(read_data));
    read_len = 0;
    flush_calls = 0;
}

static void assert_dap_cleared(const cmsis_dap_t *dap) {
    const uint8_t *bytes = (const uint8_t *)dap;
    for (size_t i = 0; i < sizeof(*dap); ++i) ASSERT(bytes[i] == 0U);
}

static cmsis_dap_t make_dap(bool bulk, int report_size) {
    cmsis_dap_t dap;
    memset(&dap, 0x5A, sizeof(dap));
    static struct hid_device device;
    dap.dev = &device;
    dap.is_bulk = bulk;
    dap.report_size = report_size;
    return dap;
}

static void test_post_reset_bulk_is_transport_only(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(true, 64);

    cmsis_dap_close_after_reset(&dap);

    ASSERT(write_calls == 0);
    ASSERT(read_calls == 0);
    ASSERT(flush_calls == 0);
    assert_dap_cleared(&dap);
    printf("test_post_reset_bulk_is_transport_only: OK\n");
}

static void test_post_reset_hid_is_transport_only(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(false, 65);

    cmsis_dap_close_after_reset(&dap);

    ASSERT(write_calls == 0);
    ASSERT(read_calls == 0);
    ASSERT(flush_calls == 0);
    assert_dap_cleared(&dap);
    printf("test_post_reset_hid_is_transport_only: OK\n");
}

static void test_null_device_only_clears_state(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(true, 64);
    dap.dev = NULL;

    cmsis_dap_close_after_reset(&dap);

    ASSERT(write_calls == 0);
    ASSERT(read_calls == 0);
    assert_dap_cleared(&dap);
    printf("test_null_device_only_clears_state: OK\n");
}

static void test_regular_bulk_disconnect(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(true, 64);
    read_data[0] = DAP_CMD_DISCONNECT;
    read_data[1] = DAP_OK;
    read_len = 2;

    ASSERT(cmsis_dap_disconnect(&dap) == NRF_OCD_OK);
    ASSERT(write_calls == 1);
    ASSERT(read_calls == 1);
    printf("test_regular_bulk_disconnect: OK\n");
}

static void test_regular_hid_disconnect(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(false, 65);
    read_data[0] = 0;
    read_data[1] = DAP_CMD_DISCONNECT;
    read_data[2] = DAP_OK;
    read_len = 3;

    ASSERT(cmsis_dap_disconnect(&dap) == NRF_OCD_OK);
    ASSERT(write_calls == 1);
    ASSERT(read_calls == 1);
    printf("test_regular_hid_disconnect: OK\n");
}

static void test_regular_hid_disconnect_without_report_id(void) {
    reset_observation();
    cmsis_dap_t dap = make_dap(false, 64);
    read_data[0] = DAP_CMD_DISCONNECT;
    read_data[1] = DAP_OK;
    read_len = 2;

    ASSERT(cmsis_dap_disconnect(&dap) == NRF_OCD_OK);
    ASSERT(write_calls == 1);
    ASSERT(read_calls == 1);
    printf("test_regular_hid_disconnect_without_report_id: OK\n");
}

int main(void) {
    test_post_reset_bulk_is_transport_only();
    test_post_reset_hid_is_transport_only();
    test_null_device_only_clears_state();
    test_regular_bulk_disconnect();
    test_regular_hid_disconnect();
    test_regular_hid_disconnect_without_report_id();
    cmsis_dap_close_after_reset(NULL);
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All post-reset CMSIS-DAP close tests passed\n");
    return 0;
}
