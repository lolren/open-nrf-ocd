#include "hid_descriptor.h"

#include <assert.h>
#include <stdio.h>

static void test_single_input_report(void) {
    const uint8_t descriptor[] = {
        0x75, 0x08,       /* Report Size: 8 bits. */
        0x95, 0x40,       /* Report Count: 64. */
        0x81, 0x02,       /* Input. */
    };
    assert(hid_input_report_size(descriptor, sizeof(descriptor), 99) == 64);
}

static void test_report_id_and_accumulated_inputs(void) {
    const uint8_t descriptor[] = {
        0x85, 0x02,       /* Report ID: 2. */
        0x75, 0x08,       /* Report Size: 8 bits. */
        0x95, 0x20,       /* Report Count: 32. */
        0x81, 0x02,       /* Input. */
        0x95, 0x1F,       /* Report Count: 31. */
        0x81, 0x02,       /* Input. */
    };
    assert(hid_input_report_size(descriptor, sizeof(descriptor), 99) == 64);
}

static void test_malformed_descriptors_use_fallback(void) {
    const uint8_t truncated_short[] = {0x75};
    const uint8_t truncated_long[] = {0xFE, 0x04, 0x01, 0x00};
    assert(hid_input_report_size(truncated_short, sizeof(truncated_short), 99) == 99);
    assert(hid_input_report_size(truncated_long, sizeof(truncated_long), 99) == 99);
}

static void test_unreasonable_report_size_uses_fallback(void) {
    const uint8_t descriptor[] = {
        0x77, 0xFF, 0xFF, 0xFF, 0xFF,
        0x97, 0xFF, 0xFF, 0xFF, 0xFF,
        0x81, 0x02,
    };
    assert(hid_input_report_size(descriptor, sizeof(descriptor), 99) == 99);
}

int main(void) {
    test_single_input_report();
    test_report_id_and_accumulated_inputs();
    test_malformed_descriptors_use_fallback();
    test_unreasonable_report_size_uses_fallback();
    puts("All HID descriptor tests passed");
    return 0;
}
