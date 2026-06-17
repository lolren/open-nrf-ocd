/* target.h - target base class.
 *
 * Encapsulates the connected CMSIS-DAP probe + the active DAP/AP state.
 * Target-specific subclasses (nrf54l, nrf54lm20a) plug in their own
 * memory map, security checks, and reset hooks.
 *
 * Mirrors the parts of pyOCD's CoreSightTarget that we actually use.
 */
#ifndef NRF_OCD_TARGET_H
#define NRF_OCD_TARGET_H

#include "cmsis_dap.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TARGET_NRF54L15,
    TARGET_NRF54LM20A,
    TARGET_UNKNOWN,
} target_type_t;

typedef enum {
    TARGET_RESET_DEFAULT = 0,
    TARGET_RESET_HALT,
    TARGET_RESET_PRE_RESET,
    TARGET_RESET_UNDER_RESET,
} target_reset_mode_t;

/* Forward declaration of the vtable. */
struct target_ops;

typedef struct {
    cmsis_dap_t        dap;
    bool               is_bulk;   /* Set by the probe layer. */
    const struct target_ops *ops;
    target_type_t      type;
    char               part_number[64];
    uint32_t           dp_idcode;
    uint32_t           ap_idr[4];
    /* Debug exception and monitor control register (DEMCR) state - we
     * need to be able to halt on reset. */
    uint32_t           demcr;
    /* Whether the target is currently halted. */
    bool               halted;
} target_t;

struct target_ops {
    nrf_ocd_status_t (*init)(target_t *t);
    nrf_ocd_status_t (*check_security)(target_t *t);
    nrf_ocd_status_t (*mass_erase)(target_t *t);
    nrf_ocd_status_t (*read_part_info)(target_t *t);
    nrf_ocd_status_t (*reset)(target_t *t, target_reset_mode_t mode);
    nrf_ocd_status_t (*halt)(target_t *t);
    nrf_ocd_status_t (*resume)(target_t *t);
};

/* Public API used by the rest of the program. */
nrf_ocd_status_t target_open(target_t *t, hid_device_t *dev, target_type_t type);
void             target_close(target_t *t);

/* Re-run the init sequence for the active target. This must be called
 * after opening the probe and before any DAP/AP operations. */
nrf_ocd_status_t target_init(target_t *t);

/* Convenience wrappers around dap_mem_read/write using AHB-AP #0. */
nrf_ocd_status_t target_mem_read(target_t *t, uint32_t addr, void *out, size_t n);
nrf_ocd_status_t target_mem_write(target_t *t, uint32_t addr, const void *in, size_t n);
nrf_ocd_status_t target_mem_write_u32(target_t *t, uint32_t addr, uint32_t value);
nrf_ocd_status_t target_mem_read_u32(target_t *t, uint32_t addr, uint32_t *value);

const char *target_type_name(target_type_t t);
target_type_t target_type_from_string(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_TARGET_H */
