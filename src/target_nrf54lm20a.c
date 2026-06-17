/* target_nrf54lm20a.c - nRF54LM20A target implementation.
 *
 * Same architecture as nRF54L15 but with a different flash controller
 * (RRAMC at 0x5004E000 vs PFU at 0x5004B000) and a different memory map.
 *
 * Memory map:
 *   0x0000_0000 - 0x001FD000  Flash (2036 KB)
 *   0x00FFD000 - 0x00FFE000  UICR (4 KB)
 *   0x2000_0000 - 0x2008_0000 RAM (512 KB)
 *   0xE00F_E000 - 0xE010_0000 ROM table (1 KB)
 */
#include "target.h"
#include "cmsis_dap.h"
#include "dap.h"
#include "log.h"
#include "nrf_ocd.h"
#include "swd.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

/* Reuse the same CTRL-AP registers and security logic as the nRF54L15
 * target - only the NVMC base and memory map differ. */
#define AHB_AP_NUM  0x0
#define CTRL_AP_NUM 0x2

/* UICR addresses. */
#define UICR_APPROTECT   0x00FFC000
#define UICR_PARTNO      0x00FFC31C
#define UICR_VARIANT     0x00FFC320

/* RRAMC registers. */
#define RRAMC_BASE          0x5004E000U
#define RRAMC_READY         (RRAMC_BASE + 0x400)
#define RRAMC_CONFIG        (RRAMC_BASE + 0x504)
#define RRAMC_ERASEPAGE     (RRAMC_BASE + 0x508)
#define RRAMC_ERASEALL      (RRAMC_BASE + 0x50C)
#define RRAMC_ERASEALLSTATUS (RRAMC_BASE + 0x510)

#define RRAMC_CONFIG_WEN    0x1
#define RRAMC_CONFIG_EEN    0x2
#define RRAMC_CONFIG_REN    0x0

static nrf_ocd_status_t rramc_wait_ready(target_t *t, uint32_t timeout_ms) {
    uint64_t deadline = nrf_ocd_monotonic_ms() + timeout_ms;
    while (nrf_ocd_monotonic_ms() < deadline) {
        uint32_t v;
        nrf_ocd_status_t st = target_mem_read_u32(t, RRAMC_READY, &v);
        if (st != NRF_OCD_OK) return st;
        if (v & 0x1) return NRF_OCD_OK;
        nrf_ocd_sleep_ms(1);
    }
    return NRF_OCD_ERR_TIMEOUT;
}

nrf_ocd_status_t rramc_config(target_t *t, uint32_t mode) {
    nrf_ocd_status_t st = rramc_wait_ready(t, 1000);
    if (st != NRF_OCD_OK) return st;
    return target_mem_write_u32(t, RRAMC_CONFIG, mode);
}

/* The init / security / reset / halt / resume code is identical to
 * nRF54L15. We delegate to those implementations to avoid duplicating
 * the CTRL-AP dance. */
extern nrf_ocd_status_t target_nrf54l_init(target_t *t);
extern nrf_ocd_status_t target_nrf54l_check_security(target_t *t);
extern nrf_ocd_status_t target_nrf54l_mass_erase(target_t *t);
extern nrf_ocd_status_t target_nrf54l_reset(target_t *t, target_reset_mode_t mode);
extern nrf_ocd_status_t target_nrf54l_halt(target_t *t);
extern nrf_ocd_status_t target_nrf54l_resume(target_t *t);

nrf_ocd_status_t target_nrf54lm20a_read_part_info(target_t *t) {
    nrf_ocd_status_t st = dap_dp_write(&t->dap, 0x8, AHB_AP_NUM);
    if (st != NRF_OCD_OK) return st;
    uint32_t partno = 0, variant = 0;
    st = target_mem_read_u32(t, UICR_PARTNO, &partno);
    if (st != NRF_OCD_OK) {
        LOG_WARNING("Could not read UICR.PARTNO: %s", nrf_ocd_strerror(st));
        return st;
    }
    st = target_mem_read_u32(t, UICR_VARIANT, &variant);
    if (st != NRF_OCD_OK) {
        LOG_WARNING("Could not read UICR.VARIANT: %s", nrf_ocd_strerror(st));
        return st;
    }
    char variant_str[5] = {0};
    variant_str[0] = (char)(variant & 0xFF);
    variant_str[1] = (char)((variant >> 8) & 0xFF);
    variant_str[2] = (char)((variant >> 16) & 0xFF);
    variant_str[3] = (char)((variant >> 24) & 0xFF);
    snprintf(t->part_number, sizeof(t->part_number), "nRF%04X%s",
             (unsigned)(partno & 0xFFFF), variant_str);
    printf("  Target: %s\n", t->part_number);
    return NRF_OCD_OK;
}

const struct target_ops target_ops_nrf54lm20a = {
    .init              = target_nrf54l_init,
    .check_security    = target_nrf54l_check_security,
    .mass_erase        = target_nrf54l_mass_erase,
    .read_part_info    = target_nrf54lm20a_read_part_info,
    .reset             = target_nrf54l_reset,
    .halt              = target_nrf54l_halt,
    .resume            = target_nrf54l_resume,
};
