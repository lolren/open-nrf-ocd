/* test_target_close.c - target teardown mode regression tests. */
#include "cmsis_dap.h"
#include "dap.h"
#include "hid.h"
#include "target.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int graceful_close_calls = 0;
static int post_reset_close_calls = 0;
static int hid_close_calls = 0;
static hid_device_t *last_closed_device = NULL;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

const struct target_ops target_ops_nrf54l15 = {0};
const struct target_ops target_ops_nrf54lm20a = {0};

nrf_ocd_status_t cmsis_dap_open(cmsis_dap_t *dap, hid_device_t *dev) {
    (void)dap;
    (void)dev;
    return NRF_OCD_OK;
}

void cmsis_dap_close(cmsis_dap_t *dap) {
    graceful_close_calls++;
    memset(dap, 0, sizeof(*dap));
}

void cmsis_dap_close_after_reset(cmsis_dap_t *dap) {
    post_reset_close_calls++;
    memset(dap, 0, sizeof(*dap));
}

void hid_close(hid_device_t *dev) {
    hid_close_calls++;
    last_closed_device = dev;
}

nrf_ocd_status_t dap_mem_read(cmsis_dap_t *dap, uint8_t ap,
                              uint32_t addr, void *buf, size_t len) {
    (void)dap;
    (void)ap;
    (void)addr;
    (void)buf;
    (void)len;
    return NRF_OCD_OK;
}

nrf_ocd_status_t dap_mem_write(cmsis_dap_t *dap, uint8_t ap,
                               uint32_t addr, const void *buf, size_t len) {
    (void)dap;
    (void)ap;
    (void)addr;
    (void)buf;
    (void)len;
    return NRF_OCD_OK;
}

int nrf_ocd_strcasecmp(const char *a, const char *b) {
    if (a == NULL || b == NULL) return (a == b) ? 0 : (a == NULL ? -1 : 1);
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void reset_observation(void) {
    graceful_close_calls = 0;
    post_reset_close_calls = 0;
    hid_close_calls = 0;
    last_closed_device = NULL;
}

static void assert_target_cleared(const target_t *target) {
    const unsigned char *bytes = (const unsigned char *)target;
    for (size_t i = 0; i < sizeof(*target); ++i) {
        ASSERT(bytes[i] == 0U);
    }
}

static void test_graceful_close(void) {
    reset_observation();
    target_t target;
    memset(&target, 0xA5, sizeof(target));
    hid_device_t *device = (hid_device_t *)(uintptr_t)0x1234U;
    target.dap.dev = device;

    target_close(&target);

    ASSERT(graceful_close_calls == 1);
    ASSERT(post_reset_close_calls == 0);
    ASSERT(hid_close_calls == 1);
    ASSERT(last_closed_device == device);
    assert_target_cleared(&target);
    printf("test_graceful_close: OK\n");
}

static void test_post_reset_transport_close(void) {
    reset_observation();
    target_t target;
    memset(&target, 0x5A, sizeof(target));
    hid_device_t *device = (hid_device_t *)(uintptr_t)0x5678U;
    target.dap.dev = device;

    target_close_after_reset(&target);

    ASSERT(graceful_close_calls == 0);
    ASSERT(post_reset_close_calls == 1);
    ASSERT(hid_close_calls == 1);
    ASSERT(last_closed_device == device);
    assert_target_cleared(&target);
    printf("test_post_reset_transport_close: OK\n");
}

static void test_null_close(void) {
    reset_observation();
    target_close(NULL);
    target_close_after_reset(NULL);
    ASSERT(graceful_close_calls == 0);
    ASSERT(post_reset_close_calls == 0);
    ASSERT(hid_close_calls == 0);
    printf("test_null_close: OK\n");
}

int main(void) {
    test_graceful_close();
    test_post_reset_transport_close();
    test_null_close();
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All target close tests passed\n");
    return 0;
}
