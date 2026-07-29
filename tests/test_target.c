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

static void test_onboard_probe_compatibility(void) {
    ASSERT(target_type_from_onboard_probe(0x2886, 0x0066) ==
           TARGET_NRF54L15);
    ASSERT(target_type_from_onboard_probe(0x2886, 0x0068) ==
           TARGET_NRF54LM20A);
    ASSERT(target_type_from_onboard_probe(0x2886, 0x9999) ==
           TARGET_UNKNOWN);
    ASSERT(target_type_from_onboard_probe(0x0D28, 0x0066) ==
           TARGET_UNKNOWN);

    ASSERT(target_matches_onboard_probe(TARGET_NRF54L15, 0x2886, 0x0066));
    ASSERT(!target_matches_onboard_probe(TARGET_NRF54LM20A, 0x2886, 0x0066));
    ASSERT(target_matches_onboard_probe(TARGET_NRF54LM20A, 0x2886, 0x0068));
    ASSERT(!target_matches_onboard_probe(TARGET_NRF54L15, 0x2886, 0x0068));

    /* External and unrecognised probes cannot identify the attached target. */
    ASSERT(target_matches_onboard_probe(TARGET_NRF54L15, 0x0D28, 0x0204));
    ASSERT(target_matches_onboard_probe(TARGET_NRF54LM20A, 0x0D28, 0x0204));
    ASSERT(target_matches_onboard_probe(TARGET_NRF54L15, 0x2886, 0x9999));
    printf("test_onboard_probe_compatibility: OK\n");
}

int main(void) {
    test_lookup();
    test_onboard_probe_compatibility();
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All target tests passed\n");
    return 0;
}
