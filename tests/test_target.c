/* test_target.c - unit tests for target type lookup. */
#include "target.h"
#include "nrf_ocd.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_lookup(void) {
    ASSERT(target_type_from_string("nrf54l15") == TARGET_NRF54L15);
    ASSERT(target_type_from_string("nrf54l") == TARGET_NRF54L15);
    ASSERT(target_type_from_string("NRF54LM20A") == TARGET_NRF54LM20A);
    ASSERT(target_type_from_string("nrf54lm20b") == TARGET_NRF54LM20A);
    ASSERT(target_type_from_string("unknown") == TARGET_UNKNOWN);
    ASSERT(target_type_from_string(NULL) == TARGET_UNKNOWN);

    ASSERT(strcmp(target_type_name(TARGET_NRF54L15), "nrf54l15") == 0);
    ASSERT(strcmp(target_type_name(TARGET_NRF54LM20A), "nrf54lm20a") == 0);
    printf("test_lookup: OK\n");
}

int main(void) {
    test_lookup();
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All target tests passed\n");
    return 0;
}
