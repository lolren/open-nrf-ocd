/* target_nrf54l.c - nRF54L15 target implementation.
 *
 * Implements the parts of pyOCD's NRF54L class that we need:
 *   - DP/AP #2 (CTRL-AP) for ERASEALL + APPROTECT.
 *   - Part info read (UICR).
 *   - Mass erase via CTRL-AP.
 *   - Reset.
 *
 * Memory map:
 *   0x0000_0000 - 0x0017D000  Flash (1.5MB)
 *   0x00FFD000 - 0x00FFE000  UICR (4KB)
 *   0x2000_0000 - 0x2004_0000 RAM (256KB)
 *   0xE00F_E000 - 0xE010_0000 ROM table (1KB)
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

/* CTRL-AP register indices. */
#define CTRL_AP_IDR                   0xFC
#define CTRL_AP_RESET                 0x000
#define CTRL_AP_ERASEALL              0x004
#define CTRL_AP_ERASEALLSTATUS        0x008
#define CTRL_AP_ERASEPROTECTSTATUS    0x00C
#define CTRL_AP_ERASEPROTECTDISABLE   0x010
#define CTRL_AP_APPROTECTSTATUS       0x014

#define CTRL_AP_ERASEALLSTATUS_READY         0x0
#define CTRL_AP_ERASEALLSTATUS_READYTORESET  0x1
#define CTRL_AP_ERASEALLSTATUS_BUSY          0x2
#define CTRL_AP_ERASEALLSTATUS_ERROR         0x3

#define CTRL_AP_APPROTECTSTATUS_APPROTECT    0x1
#define CTRL_AP_APPROTECTSTATUS_SECUREAPPROTECT 0x2

#define CTRL_AP_IDR_EXPECTED 0x32880000U

#define AHB_AP_NUM  0x0
#define CTRL_AP_NUM 0x2

/* UICR addresses. */
#define UICR_APPROTECT   0x00FFC000
#define UICR_PARTNO      0x00FFC31C
#define UICR_VARIANT     0x00FFC320
#define UICR_DEVICEADDR0 0x00FFC3A4
#define UICR_DEVICEADDR1 0x00FFC3A8

/* NVMC. */
#define NVMC_BASE        0x5004B000U
#define NVMC_READY       (NVMC_BASE + 0x400)
#define NVMC_CONFIG      (NVMC_BASE + 0x504)
#define NVMC_ERASEPAGE   (NVMC_BASE + 0x508)
#define NVMC_ERASEALL    (NVMC_BASE + 0x50C)
#define NVMC_ERASEALLSTATUS (NVMC_BASE + 0x510)

#define NVMC_CONFIG_WEN  0x1
#define NVMC_CONFIG_EEN  0x2
#define NVMC_CONFIG_REN  0x0

#define MASS_ERASE_TIMEOUT_MS 30000

/* Shared with the LM20A target implementation. */
nrf_ocd_status_t target_nrf54l_init(target_t *t);
nrf_ocd_status_t target_nrf54l_check_security(target_t *t);
nrf_ocd_status_t target_nrf54l_mass_erase(target_t *t);
nrf_ocd_status_t target_nrf54l_reset(target_t *t, target_reset_mode_t mode);
nrf_ocd_status_t target_nrf54l_halt(target_t *t);
nrf_ocd_status_t target_nrf54l_resume(target_t *t);

/* Wait for NVMC_READY to be 1. */
static nrf_ocd_status_t nvmc_wait_ready(target_t *t, uint32_t timeout_ms) {
    uint64_t deadline = nrf_ocd_monotonic_ms() + timeout_ms;
    while (nrf_ocd_monotonic_ms() < deadline) {
        uint32_t v;
        nrf_ocd_status_t st = target_mem_read_u32(t, NVMC_READY, &v);
        if (st != NRF_OCD_OK) return st;
        if (v & 0x1) return NRF_OCD_OK;
        nrf_ocd_sleep_ms(1);
    }
    return NRF_OCD_ERR_TIMEOUT;
}

static nrf_ocd_status_t nvmc_config(target_t *t, uint32_t mode) {
    nrf_ocd_status_t st = nvmc_wait_ready(t, 1000);
    if (st != NRF_OCD_OK) return st;
    return target_mem_write_u32(t, NVMC_CONFIG, mode);
}

static nrf_ocd_status_t ctrl_ap_read(target_t *t, uint8_t reg, uint32_t *value) {
    uint32_t select = (CTRL_AP_NUM << 24) | ((uint32_t)(reg & 0xF0) << 0);
    nrf_ocd_status_t st = dap_dp_write(&t->dap, 0x8, select);
    if (st != NRF_OCD_OK) return st;
    return cmsis_dap_transfer(&t->dap, DAP_TRANSFER_APnDP_AP, reg >> 2, true, value);
}

static nrf_ocd_status_t ctrl_ap_write(target_t *t, uint8_t reg, uint32_t value) {
    uint32_t select = (CTRL_AP_NUM << 24) | ((uint32_t)(reg & 0xF0) << 0);
    nrf_ocd_status_t st = dap_dp_write(&t->dap, 0x8, select);
    if (st != NRF_OCD_OK) return st;
    return cmsis_dap_transfer(&t->dap, DAP_TRANSFER_APnDP_AP, reg >> 2, false, &value);
}

/* Read DP_TARGETID (register 0x4 in DP bank 1). pyOCD uses this to
 * identify Nordic devices: bits 11:0 = 0x289, bits 19:16 = 0xC. */
static nrf_ocd_status_t read_dp_targetid(target_t *t, uint32_t *targetid) {
    nrf_ocd_status_t st = dap_dp_write(&t->dap, 0x8, 1 << 4);  /* bank 1 */
    if (st != NRF_OCD_OK) return st;
    st = dap_dp_read(&t->dap, 0x4, targetid);
    if (st != NRF_OCD_OK) return st;
    /* Restore bank 0. */
    return dap_dp_write(&t->dap, 0x8, 0);
}

static nrf_ocd_status_t target_nrf54l_swd_connect(target_t *t) {
    /* Matches the EXACT sequence from the pre-built binary's nrf_swd_connect(). */
    nrf_ocd_status_t st;
    uint32_t ctrl_stat, idr;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) nrf_ocd_sleep_ms(50 * (1 << attempt));

        /* Step 1: DAP_Connect SWD. */
        /* pyOCD connect-under-reset (port=3 = SWD+nRESET) prevents
         * boot ROM from re-locking APPROTECT after mass erase. */
        st = cmsis_dap_connect(&t->dap, DAP_PORT_SWD);
        if (st != NRF_OCD_OK) {
            st = cmsis_dap_connect(&t->dap, 3);
        }
        if (st != NRF_OCD_OK) continue;

        /* Step 2: Set SWJ clock first. pyOCD sets the clock before connect;
         * the XIAO SAMD11 bridge bit-bangs SWD and does not reliably reach
         * 4 MHz (which makes the target return NO_ACK). Use 1 MHz (pyOCD). */
        st = cmsis_dap_swj_clock(&t->dap, 1000000);
        if (st != NRF_OCD_OK) continue;

        /* Step 4: Transfer_Configure (idle=2, wait_retry=150, match=0). */
        st = cmsis_dap_transfer_configure(&t->dap, 2, 150, 0);
        if (st != NRF_OCD_OK) continue;

        /* Step 5: Full SWJ switch to SWD (line reset + JTAG-to-SWD select
         * + line reset + idle), exactly like pyOCD's switch_to_swd(). The
         * DP must be in SWD mode before the first transfer or it returns
         * NO_ACK. */
        st = swd_switch_to_swd(&t->dap);
        if (st != NRF_OCD_OK) continue;

        /* Read DP IDCODE to exit line-reset state. */
        st = swd_read_idcode(&t->dap, &idr);
        if (st != NRF_OCD_OK) continue;
        if (idr == 0 || idr == 0xFFFFFFFFU) continue;
        t->dp_idcode = idr;
        LOG_INFO("DP IDCODE: 0x%08x", idr);

        /* Step 6: DP_SELECT = bank 0. */
        st = dap_dp_write(&t->dap, 0x8, 0x00000000);
        if (st != NRF_OCD_OK) continue;

        /* Step 7: Clear sticky errors via DP_ABORT. */
        st = dap_dp_write(&t->dap, 0x0, 0x0000001FU);
        if (st != NRF_OCD_OK) continue;

        /* Step 8: Power up debug + system domains with MASKLANE. */
        st = dap_dp_write(&t->dap, 0x4,
                          (1U << 28) | (1U << 30) | 0x00000F00U);
        if (st != NRF_OCD_OK) continue;

        /* Step 9: Poll for power-up acknowledge. */
        uint64_t deadline = nrf_ocd_monotonic_ms() + 2000;
        bool powered = false;
        while (nrf_ocd_monotonic_ms() < deadline) {
            st = dap_dp_read(&t->dap, 0x4, &ctrl_stat);
            if (st != NRF_OCD_OK) break;
            if ((ctrl_stat & ((1U << 29) | (1U << 31))) == ((1U << 29) | (1U << 31))) {
                powered = true;
                break;
            }
            nrf_ocd_sleep_ms(10);
        }
        if (!powered) {
            LOG_DEBUG("Power-up not ack'd on attempt %d", attempt + 1);
            continue;
        }
        
        /* Step 10 (pyOCD): Halt the Cortex-M core via DHCSR so
         * AHB-AP memory reads (UICR, flash) don't FAULT. */
        for (int hi = 0; hi < 3; hi++) {
            st = target_mem_write_u32(t, 0xE000EDF0, 0xA05F0003);
            if (st == NRF_OCD_OK) {
                t->halted = true;
                break;
            }
            nrf_ocd_sleep_ms(50);
            /* Clear sticky errors and retry. */
            st = dap_dp_write(&t->dap, 0x0, 0x0000001FU);
            if (st != NRF_OCD_OK) continue;
            /* Re-power-up after clearing errors. */
            st = dap_dp_write(&t->dap, 0x4,
                              (1U << 28) | (1U << 30) | 0x00000F00U);
            if (st != NRF_OCD_OK) continue;
        }
        if (t->halted) break;
    }
    if (st != NRF_OCD_OK) goto init_failed;

    /* Try to read CTRL-AP IDR. */
    {
        uint32_t cidr = 0;
        st = ctrl_ap_read(t, CTRL_AP_IDR, &cidr);
        st = ctrl_ap_read(t, CTRL_AP_IDR, &cidr);
        LOG_DEBUG("CTRL-AP IDR: 0x%08x", cidr);
    if (cidr != CTRL_AP_IDR_EXPECTED) {
        LOG_WARNING("CTRL-AP IDR mismatch: got 0x%08x expected 0x%08x; continuing anyway",
                     cidr, CTRL_AP_IDR_EXPECTED);
    }
    /* Switch back to AHB-AP (AP #0). */
    st = dap_dp_write(&t->dap, 0x8, AHB_AP_NUM);
    }
init_failed:
    return st;
}

nrf_ocd_status_t target_nrf54l_init(target_t *t) {
    return target_nrf54l_swd_connect(t);
}

nrf_ocd_status_t target_nrf54l_check_security(target_t *t) {
    /* If the AHB-AP is enabled, the target is open. Otherwise, check
     * APPROTECT and offer to mass-erase. */
    uint32_t csw = 0;
    nrf_ocd_status_t st = cmsis_dap_transfer(&t->dap, DAP_TRANSFER_APnDP_AP, 0x0,
                                              true, &csw);
    if (st != NRF_OCD_OK) return st;
    if (csw & CSW_DEVICEEN) {
        LOG_INFO("Target is not in a secure state (CSW.DeviceEn=1)");
        return NRF_OCD_OK;
    }
    if (!t->allow_mass_erase) {
        LOG_ERROR("APPROTECT is enabled; use --auto-unlock or request a chip erase to unlock");
        return NRF_OCD_ERR_LOCKED;
    }
    LOG_WARNING("APPROTECT enabled: mass-erasing target to unlock");
    return target_nrf54l_mass_erase(t);
}

nrf_ocd_status_t target_nrf54l_mass_erase(target_t *t) {
    /* Switch to CTRL-AP. */
    nrf_ocd_status_t st = dap_dp_write(&t->dap, 0x8, CTRL_AP_NUM);
    if (st != NRF_OCD_OK) return st;
    /* Trigger ERASEALL. */
    st = ctrl_ap_write(t, CTRL_AP_ERASEALL >> 2, 0x1);
    if (st != NRF_OCD_OK) return st;
    /* Poll for ERASEALLSTATUS != READY (indicates erase has started).
     * On some boards the erase completes so fast we skip BUSY and go
     * directly to READYTORESET — accept either. */
    uint64_t deadline = nrf_ocd_monotonic_ms() + MASS_ERASE_TIMEOUT_MS;
    uint32_t status = 0;
    while (nrf_ocd_monotonic_ms() < deadline) {
        st = ctrl_ap_read(t, CTRL_AP_ERASEALLSTATUS >> 2, &status);
        if (st != NRF_OCD_OK) return st;
        if (status != CTRL_AP_ERASEALLSTATUS_READY) break;
        nrf_ocd_sleep_ms(50);
    }
    if (status == CTRL_AP_ERASEALLSTATUS_READY) {
        LOG_ERROR("Mass erase timeout: ERASEALLSTATUS still READY");
        return NRF_OCD_ERR_TIMEOUT;
    }
    LOG_DEBUG("ERASEALLSTATUS=0x%x after trigger", status);
    /* Now wait for READYTORESET. */
    deadline = nrf_ocd_monotonic_ms() + MASS_ERASE_TIMEOUT_MS;
    while (nrf_ocd_monotonic_ms() < deadline) {
        st = ctrl_ap_read(t, CTRL_AP_ERASEALLSTATUS >> 2, &status);
        if (st != NRF_OCD_OK) return st;
        if (status == CTRL_AP_ERASEALLSTATUS_READYTORESET) break;
        nrf_ocd_sleep_ms(50);
    }
    if (status != CTRL_AP_ERASEALLSTATUS_READYTORESET) {
        LOG_ERROR("Mass erase timeout waiting for ERASEALLSTATUS=READYTORESET (got 0x%x)",
                  status);
        return NRF_OCD_ERR_TIMEOUT;
    }
    nrf_ocd_sleep_ms(10);
    /* Reset. */
    st = ctrl_ap_write(t, CTRL_AP_RESET >> 2, 0x2);
    if (st != NRF_OCD_OK) return st;
    st = ctrl_ap_write(t, CTRL_AP_RESET >> 2, 0x0);
    if (st != NRF_OCD_OK) return st;
    nrf_ocd_sleep_ms(200);
    /* Re-init SWD because the target has rebooted. */
    st = swd_line_reset(&t->dap);
    if (st != NRF_OCD_OK) return st;
    /* Power-up the debug domain again. */
    st = target_nrf54l_init(t);
    if (st != NRF_OCD_OK) return st;
    t->mass_erased = true;
    LOG_INFO("Mass erase complete");
    return NRF_OCD_OK;
}

nrf_ocd_status_t target_nrf54l_read_part_info(target_t *t) {
    /* Switch to AHB-AP. */
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

nrf_ocd_status_t target_nrf54l_reset(target_t *t, target_reset_mode_t mode) {
    /* For under-reset mode, use CMSIS-DAP SWJ_PINS to toggle nRESET. */
    if (mode == TARGET_RESET_UNDER_RESET) {
        uint8_t pins;
        nrf_ocd_status_t st = cmsis_dap_swj_pins(&t->dap, 0, &pins, 0);
        if (st != NRF_OCD_OK) return st;
        /* nRESET = bit 7, SWCLK = bit 1, SWDIO = bit 0 */
        st = cmsis_dap_swj_pins(&t->dap, 0x80, &pins, 100);  /* nRESET low for 100ms */
        if (st != NRF_OCD_OK) return st;
        st = cmsis_dap_swj_pins(&t->dap, 0x00, &pins, 10);   /* nRESET high */
        if (st != NRF_OCD_OK) return st;
        t->halted = false;
        return NRF_OCD_OK;
    }
    
    /* The nRF54L reset is a CPU-side AIRCR reset. Issue VECTKEY+SYSRESETREQ
     * to the AHB-AP. */
    if (mode == TARGET_RESET_HALT) {
        /* Halt the core on reset: enable TRCENA in DEMCR + set VC_CORERESET. */
        nrf_ocd_status_t st = target_mem_read_u32(t, 0xE000EDFC, &t->demcr);
        if (st != NRF_OCD_OK) return st;
        st = target_mem_write_u32(t, 0xE000EDFC, t->demcr | (1U << 0));
        if (st != NRF_OCD_OK) return st;
        /* Set C_DEBUGEN to halt on reset. */
        st = target_mem_write_u32(t, 0xE000ED30, 0xA05F0003);  /* DHCSR: C_DEBUGEN | C_HALT */
        if (st != NRF_OCD_OK) return st;
    }
    /* Issue SYSRESETREQ. */
    nrf_ocd_status_t st = target_mem_write_u32(t, 0xE000ED0C, 0x05FA0004);
    if (st != NRF_OCD_OK) return st;
    nrf_ocd_sleep_ms(50);
    t->halted = false;
    return NRF_OCD_OK;
}

nrf_ocd_status_t target_nrf54l_halt(target_t *t) {
    /* DHCSR at 0xE000EDF0 (Cortex-M33). Write 0xA05F0003 = C_DEBUGEN | C_HALT. */
    nrf_ocd_status_t st = target_mem_write_u32(t, 0xE000EDF0, 0xA05F0003);
    if (st != NRF_OCD_OK) return st;
    t->halted = true;
    return NRF_OCD_OK;
}

nrf_ocd_status_t target_nrf54l_resume(target_t *t) {
    /* Write 0xA05F0001 = DEMCR enable, C_DEBUGEN only (no HALT). */
    nrf_ocd_status_t st = target_mem_write_u32(t, 0xE000EDF0, 0xA05F0001);
    if (st != NRF_OCD_OK) return st;
    t->halted = false;
    return NRF_OCD_OK;
}

/* The vtable for the nRF54L15 target. */
const struct target_ops target_ops_nrf54l15 = {
    .init              = target_nrf54l_init,
    .check_security    = target_nrf54l_check_security,
    .mass_erase        = target_nrf54l_mass_erase,
    .read_part_info    = target_nrf54l_read_part_info,
    .reset             = target_nrf54l_reset,
    .halt              = target_nrf54l_halt,
    .resume            = target_nrf54l_resume,
};
