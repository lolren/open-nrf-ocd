/* test_cmsis_dap_transfer.c - bounded transfer recovery tests. */
#include "cmsis_dap.h"
#include "hid.h"
#include "swd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct hid_device {
    int unused;
};

static int failures;
static int write_calls;
static int read_calls;
static int line_reset_calls;
static int fallback_reset_calls;
static uint8_t responses[4][NRF_OCD_HID_REPORT_SIZE];
static size_t response_lengths[4];
static size_t response_count;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

nrf_ocd_status_t hid_write(hid_device_t *dev, const uint8_t *data, size_t len,
                           int timeout_ms) {
    (void)dev;
    (void)data;
    (void)len;
    (void)timeout_ms;
    write_calls++;
    return NRF_OCD_OK;
}

nrf_ocd_status_t hid_read(hid_device_t *dev, uint8_t *data, size_t capacity,
                          size_t *out_len, int timeout_ms) {
    (void)dev;
    (void)timeout_ms;
    if ((size_t)read_calls >= response_count) return NRF_OCD_ERR_IO;
    size_t length = response_lengths[read_calls];
    if (length > capacity) length = capacity;
    memcpy(data, responses[read_calls], length);
    *out_len = length;
    read_calls++;
    return NRF_OCD_OK;
}

void hid_flush(hid_device_t *dev) {
    (void)dev;
}

nrf_ocd_status_t swd_line_reset_swd_sequence(cmsis_dap_t *dap) {
    (void)dap;
    line_reset_calls++;
    return NRF_OCD_OK;
}

nrf_ocd_status_t swd_line_reset(cmsis_dap_t *dap) {
    (void)dap;
    fallback_reset_calls++;
    return NRF_OCD_OK;
}

static cmsis_dap_t make_dap(bool bulk) {
    static struct hid_device device;
    cmsis_dap_t dap = {0};
    dap.dev = &device;
    dap.is_bulk = bulk;
    dap.report_size = 64;
    return dap;
}

static void reset_observation(void) {
    write_calls = 0;
    read_calls = 0;
    line_reset_calls = 0;
    fallback_reset_calls = 0;
    memset(responses, 0, sizeof(responses));
    memset(response_lengths, 0, sizeof(response_lengths));
    response_count = 0;
}

static void queue_bulk(uint8_t ack, uint32_t value) {
    size_t index = response_count++;
    responses[index][0] = DAP_CMD_TRANSFER;
    responses[index][1] = 1;
    responses[index][2] = ack;
    responses[index][3] = (uint8_t)value;
    responses[index][4] = (uint8_t)(value >> 8);
    responses[index][5] = (uint8_t)(value >> 16);
    responses[index][6] = (uint8_t)(value >> 24);
    response_lengths[index] = 7;
}

static void queue_hid(uint8_t ack, uint32_t value) {
    size_t index = response_count++;
    responses[index][0] = 0;
    responses[index][1] = DAP_CMD_TRANSFER;
    responses[index][2] = 1;
    responses[index][3] = ack;
    responses[index][4] = (uint8_t)value;
    responses[index][5] = (uint8_t)(value >> 8);
    responses[index][6] = (uint8_t)(value >> 16);
    responses[index][7] = (uint8_t)(value >> 24);
    response_lengths[index] = 8;
}

static void test_bulk_no_ack_retries_once(void) {
    reset_observation();
    queue_bulk(DAP_TRANSFER_NO_ACK, 0);
    queue_bulk(DAP_TRANSFER_OK, 0x12345678U);
    cmsis_dap_t dap = make_dap(true);
    uint32_t value = 0;

    nrf_ocd_status_t status = cmsis_dap_transfer(
        &dap, DAP_TRANSFER_APnDP_DP, 0, true, &value);

    ASSERT(status == NRF_OCD_OK);
    ASSERT(value == 0x12345678U);
    ASSERT(write_calls == 2);
    ASSERT(read_calls == 2);
    ASSERT(line_reset_calls == 1);
    ASSERT(fallback_reset_calls == 0);
    printf("test_bulk_no_ack_retries_once: OK\n");
}

static void test_repeated_no_ack_is_bounded(void) {
    reset_observation();
    queue_bulk(DAP_TRANSFER_NO_ACK, 0);
    queue_bulk(DAP_TRANSFER_NO_ACK, 0);
    cmsis_dap_t dap = make_dap(true);
    uint32_t value = 0;

    nrf_ocd_status_t status = cmsis_dap_transfer(
        &dap, DAP_TRANSFER_APnDP_DP, 0, true, &value);

    ASSERT(status == NRF_OCD_ERR_PROTOCOL);
    ASSERT(write_calls == 2);
    ASSERT(read_calls == 2);
    ASSERT(line_reset_calls == 1);
    printf("test_repeated_no_ack_is_bounded: OK\n");
}

static void test_hid_wait_uses_ack_byte(void) {
    reset_observation();
    queue_hid(DAP_TRANSFER_WAIT, 0);
    queue_hid(DAP_TRANSFER_OK, 0x89ABCDEFU);
    cmsis_dap_t dap = make_dap(false);
    uint32_t value = 0;

    nrf_ocd_status_t status = cmsis_dap_transfer(
        &dap, DAP_TRANSFER_APnDP_DP, 0, true, &value);

    ASSERT(status == NRF_OCD_OK);
    ASSERT(value == 0x89ABCDEFU);
    ASSERT(write_calls == 2);
    ASSERT(read_calls == 2);
    ASSERT(line_reset_calls == 0);
    printf("test_hid_wait_uses_ack_byte: OK\n");
}

static void test_hid_no_ack_retries_once(void) {
    reset_observation();
    queue_hid(DAP_TRANSFER_NO_ACK, 0);
    queue_hid(DAP_TRANSFER_OK, 0x0BADF00DU);
    cmsis_dap_t dap = make_dap(false);
    uint32_t value = 0;

    nrf_ocd_status_t status = cmsis_dap_transfer(
        &dap, DAP_TRANSFER_APnDP_DP, 0, true, &value);

    ASSERT(status == NRF_OCD_OK);
    ASSERT(value == 0x0BADF00DU);
    ASSERT(write_calls == 2);
    ASSERT(read_calls == 2);
    ASSERT(line_reset_calls == 1);
    printf("test_hid_no_ack_retries_once: OK\n");
}

static void test_repeated_hid_no_ack_is_bounded(void) {
    reset_observation();
    queue_hid(DAP_TRANSFER_NO_ACK, 0);
    queue_hid(DAP_TRANSFER_NO_ACK, 0);
    cmsis_dap_t dap = make_dap(false);
    uint32_t value = 0;

    nrf_ocd_status_t status = cmsis_dap_transfer(
        &dap, DAP_TRANSFER_APnDP_DP, 0, true, &value);

    ASSERT(status == NRF_OCD_ERR_PROTOCOL);
    ASSERT(write_calls == 2);
    ASSERT(read_calls == 2);
    ASSERT(line_reset_calls == 1);
    printf("test_repeated_hid_no_ack_is_bounded: OK\n");
}

int main(void) {
    test_bulk_no_ack_retries_once();
    test_repeated_no_ack_is_bounded();
    test_hid_wait_uses_ack_byte();
    test_hid_no_ack_retries_once();
    test_repeated_hid_no_ack_is_bounded();
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All CMSIS-DAP transfer recovery tests passed\n");
    return 0;
}
