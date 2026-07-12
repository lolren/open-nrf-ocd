#include "hid_descriptor.h"

#include <stdbool.h>
#include <limits.h>

int hid_input_report_size(const uint8_t *descriptor, size_t descriptor_size,
                          int fallback_size) {
    if (!descriptor || fallback_size <= 0) return fallback_size;

    uint32_t report_count = 0;
    uint32_t report_size_bits = 0;
    uint8_t report_id = 0;
    bool uses_report_ids = false;
    uint64_t input_bits[256] = {0};

    for (size_t offset = 0; offset < descriptor_size;) {
        uint8_t prefix = descriptor[offset];
        if (prefix == 0xFE) {
            if (descriptor_size - offset < 3) return fallback_size;
            size_t item_size = 3U + (size_t)descriptor[offset + 1U];
            if (item_size > descriptor_size - offset) return fallback_size;
            offset += item_size;
            continue;
        }

        uint8_t size = prefix & 0x03U;
        uint8_t type = (prefix >> 2) & 0x03U;
        uint8_t tag = (prefix >> 4) & 0x0FU;
        if (size == 3U) size = 4U;
        if ((size_t)size > descriptor_size - offset - 1U) return fallback_size;

        uint32_t value = 0;
        for (uint8_t index = 0; index < size; index++) {
            value |= (uint32_t)descriptor[offset + 1U + index] << (8U * index);
        }
        offset += 1U + size;

        if (type == 1U) {
            if (tag == 0x7U) report_size_bits = value;
            if (tag == 0x8U && value > 0U && value <= UINT8_MAX) {
                report_id = (uint8_t)value;
                uses_report_ids = true;
            }
            if (tag == 0x9U) report_count = value;
        } else if (type == 0U && tag == 0x8U) {
            uint64_t item_bits = (uint64_t)report_count * report_size_bits;
            if (UINT64_MAX - input_bits[report_id] < item_bits) {
                return fallback_size;
            }
            input_bits[report_id] += item_bits;
        }
    }

    uint64_t max_bytes = 0;
    for (size_t id = 0; id < 256; id++) {
        if (input_bits[id] == 0) continue;
        if (input_bits[id] > UINT64_MAX - 7U) return fallback_size;
        uint64_t bytes = (input_bits[id] + 7U) / 8U;
        if (uses_report_ids) bytes++;
        if (bytes > max_bytes) max_bytes = bytes;
    }
    if (max_bytes == 0 || max_bytes > INT_MAX) return fallback_size;
    return (int)max_bytes;
}
