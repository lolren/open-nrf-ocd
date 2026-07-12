#ifndef NRF_OCD_HID_DESCRIPTOR_H
#define NRF_OCD_HID_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

int hid_input_report_size(const uint8_t *descriptor, size_t descriptor_size,
                          int fallback_size);

#endif /* NRF_OCD_HID_DESCRIPTOR_H */
