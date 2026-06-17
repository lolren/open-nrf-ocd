/* swd.h - SWD transport helpers.
 *
 * Most SWD line-level sequencing is delegated to CMSIS-DAP commands
 * (DAP_SWD_Configure, DAP_Transfer). What we add on top is the small
 * collection of pure-SWD side effects that pyOCD also handles:
 *   - line reset  (50 high clocks + JTAG-to-SWD sequence)
 *   - switch from JTAG to SWD
 *   - target SWDIDCODE read
 */
#ifndef NRF_OCD_SWD_H
#define NRF_OCD_SWD_H

#include "cmsis_dap.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The JTAG-to-SWD select sequence required by the ARM CoreSight
 * specification. Bit stream is sent MSB first, length = 16 bits. */
nrf_ocd_status_t swd_switch_to_swd(cmsis_dap_t *dap);

/* Drive the SWDIO line for at least 50 cycles to recover from a stuck
 * partner. Uses SWD_SEQUENCE (0x1D) for reliability. */
nrf_ocd_status_t swd_line_reset(cmsis_dap_t *dap);
nrf_ocd_status_t swd_line_reset_swd_sequence(cmsis_dap_t *dap);

/* Read DP_IDCODE. */
nrf_ocd_status_t swd_read_idcode(cmsis_dap_t *dap, uint32_t *idcode);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_SWD_H */
