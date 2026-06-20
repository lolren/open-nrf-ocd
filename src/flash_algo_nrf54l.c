/* flash_algo_nrf54l.c - Flash algorithm for nRF54L family.
 *
 * Implements flash programming by loading the position-independent flash
 * algorithm into RAM and executing it via core register manipulation,
 * exactly matching pyOCD's approach.
 *
 * Based on the flash algorithm from pyocd's target_nRF54L15.py.
 */
#include "flash_algo_nrf54l.h"
#include "dap.h"
#include "log.h"
#include "nrf_ocd.h"
#include "target.h"
#include "util.h"

#include <string.h>

/* Cortex-M core debug registers (DP/CP interface). */
#define DHCSR  0xE000EDF0
#define DCRSR  0xE000EDF4
#define DCRDR  0xE000EDF8

/* DHCSR bits. */
#define DBGKEY        (0xA05F << 16)
#define C_DEBUGEN     (1U << 0)
#define C_HALT        (1U << 1)
#define S_REGRDY      (1U << 16)
#define S_HALT        (1U << 17)

/* DCRSR bits. */
#define DCRSR_REGWnR  (1U << 16)
#define DCRSR_REGSEL  0x1F

/* Register indices for write_core_registers_raw. */
#define REG_R0        0
#define REG_R1        1
#define REG_R2        2
#define REG_R3        3
#define REG_R9        9
#define REG_SP        13
#define REG_LR        14
#define REG_PC        15
#define REG_XPSR      16
/* PRIMASK (BASEPRI alias) - write 1 to disable all configurable interrupts. */
#define REG_PRIMASK   20
#define XPSR_THUMB    0x01000000U

/* Flash algorithm from pyOCD's target_nRF54L15.py. */
static const uint32_t nrf54l15_flash_algo_instructions[] = {
    0xE00ABE00,
    0xf8d24a02, 0x2b013400, 0x4770d1fb, 0x5004b000, 0x47702000, 0x47702000, 0x49072001, 0xf8c1b508,
    0xf7ff0500, 0xf8c1ffed, 0x20000540, 0xffe8f7ff, 0x0500f8c1, 0xbf00bd08, 0x5004b000, 0x2301b508,
    0xf8c14906, 0xf7ff3500, 0xf04fffdb, 0x600333ff, 0xf7ff2000, 0xf8c1ffd5, 0xbd080500, 0x5004b000,
    0x2301b538, 0x4d0c4614, 0x0103f021, 0x3500f8c5, 0xffc6f7ff, 0x44214622, 0x42911b00, 0x2000d105,
    0xffbef7ff, 0x0500f8c5, 0x4613bd38, 0x4b04f853, 0x461a5014, 0xbf00e7f1, 0x5004b000, 0x00000000
};

/* Flash algorithm from pyOCD's target_nRF54LM20A.py (RRAMC at 0x5004E000). */
static const uint32_t nrf54lm20a_flash_algo_instructions[] = {
    0xE00ABE00,
    0xf8d24a02, 0x2b013400, 0x4770d1fb, 0x5004e000, 0x47702000, 0x47702000, 0x49072001, 0xf8c1b508,
    0xf7ff0500, 0xf8c1ffed, 0x20000540, 0xffe8f7ff, 0x0500f8c1, 0xbf00bd08, 0x5004e000, 0x2301b508,
    0xf8c14906, 0xf7ff3500, 0xf04fffdb, 0x600333ff, 0xf7ff2000, 0xf8c1ffd5, 0xbd080500, 0x5004e000,
    0x2301b538, 0x4d0c4614, 0x0103f021, 0x3500f8c5, 0xffc6f7ff, 0x44214622, 0x42911b00, 0x2000d105,
    0xffbef7ff, 0x0500f8c5, 0x4613bd38, 0x4b04f853, 0x461a5014, 0xbf00e7f1, 0x5004e000, 0x00000000
};

/* Flash algorithm metadata. */
#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))

static const flash_algo_t nrf54l15_algo = {
    .load_address = 0x20000000,
    .instructions = nrf54l15_flash_algo_instructions,
    .instruction_count = ARRAY_COUNT(nrf54l15_flash_algo_instructions),
    .pc_init = 0x20000015,
    .pc_uninit = 0x20000019,
    .pc_program_page = 0x20000065,
    .pc_erase_sector = 0x20000041,
    .pc_erase_all = 0x2000001d,
    .static_base = 0x20000000 + 0x04 + 0xA0,
    .begin_stack = 0x20000300,
    .page_size = 4,
    .page_buffers = { 0x20001000, 0x20001004 },
    .min_program_length = 4,
    .flash_start = 0x0,
    .flash_size = 0x17D000,
};

static const flash_algo_t nrf54lm20a_algo = {
    .load_address = 0x20000000,
    .instructions = nrf54lm20a_flash_algo_instructions,
    .instruction_count = ARRAY_COUNT(nrf54lm20a_flash_algo_instructions),
    .pc_init = 0x20000015,
    .pc_uninit = 0x20000019,
    .pc_program_page = 0x20000065,
    .pc_erase_sector = 0x20000041,
    .pc_erase_all = 0x2000001d,
    .static_base = 0x20000000 + 0x04 + 0xA0,
    .begin_stack = 0x20000300,
    .page_size = 4,
    .page_buffers = { 0x20001000, 0x20001004 },
    .min_program_length = 4,
    .flash_start = 0x0,
    .flash_size = 0x17D000,
};

const flash_algo_t *flash_algo_for_target(target_t *t) {
    switch (t->type) {
        case TARGET_NRF54L15:   return &nrf54l15_algo;
        case TARGET_NRF54LM20A: return &nrf54lm20a_algo;
        default:               return NULL;
    }
}

/* Write a 32-bit value to a core register via DCRSR/DCRDR.
 * The core must be halted. */
static nrf_ocd_status_t core_reg_write(target_t *t, uint8_t reg, uint32_t data) {
    cmsis_dap_t *dap = &t->dap;
    
    LOG_DEBUG("core_reg_write: reg=%d data=0x%08X", reg, data);
    /* Write DCRDR. */
    nrf_ocd_status_t st = dap_mem_write(dap, 0, DCRDR, &data, 4);
    if (st != NRF_OCD_OK) return st;
    
    /* Write DCRSR with register index and write flag. */
    uint32_t dcrsr = reg | DCRSR_REGWnR;
    st = dap_mem_write(dap, 0, DCRSR, &dcrsr, 4);
    if (st != NRF_OCD_OK) return st;
    
    /* Poll DHCSR for S_REGRDY. */
    uint64_t deadline = nrf_ocd_monotonic_ms() + 100;
    uint32_t dhcsr = 0;
    while (nrf_ocd_monotonic_ms() < deadline) {
        st = dap_mem_read(dap, 0, DHCSR, &dhcsr, 4);
        if (st != NRF_OCD_OK) return st;
        if (dhcsr & S_REGRDY) return NRF_OCD_OK;
        nrf_ocd_sleep_ms(1);
    }
    return NRF_OCD_ERR_TIMEOUT;
}

/* Read a 32-bit value from a core register via DCRSR/DCRDR. */
static nrf_ocd_status_t core_reg_read(target_t *t, uint8_t reg, uint32_t *data) {
    cmsis_dap_t *dap = &t->dap;
    
    LOG_DEBUG("core_reg_read: reg=%d", reg);
    /* Write DCRSR with register index (no write flag).
     * NOTE: Must use uint32_t - dap_mem_write reads exactly 4 bytes and
     * passing &reg (uint8_t*) would overflow into adjacent stack vars! */
    uint32_t dcrsr_val = (uint32_t)reg;
    nrf_ocd_status_t st = dap_mem_write(dap, 0, DCRSR, &dcrsr_val, 4);
    if (st != NRF_OCD_OK) return st;
    
    /* Poll DHCSR for S_REGRDY. */
    uint64_t deadline = nrf_ocd_monotonic_ms() + 100;
    uint32_t dhcsr = 0;
    while (nrf_ocd_monotonic_ms() < deadline) {
        st = dap_mem_read(dap, 0, DHCSR, &dhcsr, 4);
        if (st != NRF_OCD_OK) return st;
        if (dhcsr & S_REGRDY) break;
        nrf_ocd_sleep_ms(1);
    }
    if (!(dhcsr & S_REGRDY)) return NRF_OCD_ERR_TIMEOUT;
    
    /* Read DCRDR. */
    return dap_mem_read(dap, 0, DCRDR, data, 4);
}

/* Write multiple core registers at once (matching pyOCD's write_core_registers_raw). */
static nrf_ocd_status_t core_regs_write(target_t *t, const uint8_t *regs, const uint32_t *vals, size_t count) {
    for (size_t i = 0; i < count; i++) {
        nrf_ocd_status_t st = core_reg_write(t, regs[i], vals[i]);
        if (st != NRF_OCD_OK) return st;
    }
    return NRF_OCD_OK;
}

/* Halt the core via DHCSR. */
nrf_ocd_status_t flash_algo_halt(target_t *t) {
    LOG_DEBUG("flash_algo_halt: writing DHCSR=0x%08X", DBGKEY | C_DEBUGEN | C_HALT);
    uint32_t dhcsr = DBGKEY | C_DEBUGEN | C_HALT;
    nrf_ocd_status_t st = dap_mem_write(&t->dap, 0, DHCSR, &dhcsr, 4);
    if (st != NRF_OCD_OK) return st;
    
    /* Poll for S_HALT. */
    uint64_t deadline = nrf_ocd_monotonic_ms() + 1000;
    uint32_t val;
    while (nrf_ocd_monotonic_ms() < deadline) {
        st = dap_mem_read(&t->dap, 0, DHCSR, &val, 4);
        if (st != NRF_OCD_OK) return st;
        if (val & S_HALT) { t->halted = true; return NRF_OCD_OK; }
        nrf_ocd_sleep_ms(1);
    }
    return NRF_OCD_ERR_TIMEOUT;
}

/* Resume the core via DHCSR. */
/* DFSR bits. */
#define DFSR_BKPT     (1U << 1)
#define DFSR_HALTED   (1U << 0)

nrf_ocd_status_t flash_algo_resume(target_t *t) {
    cmsis_dap_t *dap = &t->dap;
    
    /* Clear debug halt cause bits (pyocd: clear_debug_cause_bits). */
    uint32_t dfsr = DFSR_BKPT | DFSR_HALTED;
    dap_mem_write(dap, 0, 0xE000ED30, &dfsr, 4);  /* DFSR */
    
    /* Resume the core. */
    LOG_DEBUG("flash_algo_resume: writing DHCSR=0x%08X", DBGKEY | C_DEBUGEN);
    uint32_t dhcsr = DBGKEY | C_DEBUGEN;
    nrf_ocd_status_t st = dap_mem_write(dap, 0, DHCSR, &dhcsr, 4);
    if (st != NRF_OCD_OK) { t->halted = false; return st; }
    
    /* Flush: read DHCSR to ensure the resume write completes. */
    dap_mem_read(dap, 0, DHCSR, &dhcsr, 4);
    
    t->halted = false;
    return NRF_OCD_OK;
}

/* Get core state from DHCSR. */
static nrf_ocd_status_t flash_algo_get_state(target_t *t, uint32_t *dhcsr) {
    return dap_mem_read(&t->dap, 0, DHCSR, dhcsr, 4);
}

/* Wait for the core to halt (after BKPT). */
nrf_ocd_status_t flash_algo_wait_halt(target_t *t, uint32_t timeout_ms) {
    uint64_t deadline = nrf_ocd_monotonic_ms() + timeout_ms;
    uint32_t dhcsr;
    int consecutive_errors = 0;
    int poll_count = 0;
    
    LOG_DEBUG("wait_halt: waiting up to %u ms", timeout_ms);
    while (nrf_ocd_monotonic_ms() < deadline) {
        nrf_ocd_status_t st = flash_algo_get_state(t, &dhcsr);
        if (st != NRF_OCD_OK) {
            /* Retry on transient errors (pyocd retries on WAIT/FAULT). */
            consecutive_errors++;
            if (consecutive_errors > 100) {
                LOG_DEBUG("wait_halt: too many consecutive errors");
                break;
            }
            nrf_ocd_sleep_ms(1);
            continue;
        }
        consecutive_errors = 0;
        if (dhcsr & S_HALT) {
            LOG_DEBUG("wait_halt: S_HALT detected (DHCSR=0x%08X poll=%d)", dhcsr, poll_count);
            t->halted = true;
            return NRF_OCD_OK;
        }

        nrf_ocd_sleep_ms(1);
        poll_count++;
    }
    /* Force halt on timeout. */
    flash_algo_halt(t);
    return NRF_OCD_ERR_TIMEOUT;
}

/* Call a flash algorithm function and wait for completion.
 * This matches pyOCD's _call_function_and_wait. */
nrf_ocd_status_t flash_algo_call_function(target_t *t, const flash_algo_t *algo,
                                           uint32_t pc, uint32_t r0, uint32_t r1, uint32_t r2,
                                           bool init, uint32_t timeout_ms, uint32_t *result) {
    /* Build register list: PC, R0-R3, R9 (if init), SP (if init), XPSR (if init), LR. */
    uint8_t regs[8];
    uint32_t vals[8];
    size_t count = 0;
    
    regs[count] = REG_PC;     vals[count] = pc;       count++;
    regs[count] = REG_R0;     vals[count] = r0;       count++;
    regs[count] = REG_R1;     vals[count] = r1;       count++;
    regs[count] = REG_R2;     vals[count] = r2;       count++;
    
    if (init) {
        regs[count] = REG_R9;     vals[count] = algo->static_base;  count++;
        regs[count] = REG_SP;     vals[count] = algo->begin_stack;  count++;
    }
    
    /* Always set XPSR for Thumb mode. */
    regs[count] = REG_XPSR;   vals[count] = XPSR_THUMB;         count++;
    
    /* LR = load_address + 1 (return address acts as BKPT). */
    regs[count] = REG_LR;     vals[count] = algo->load_address + 1; count++;
    
    /* Write all registers. */
    nrf_ocd_status_t st = core_regs_write(t, regs, vals, count);
    if (st != NRF_OCD_OK) return st;
    
    /* Disable all interrupts on target before resuming.
     * This prevents interrupt handlers from interfering with flash writes. */
    uint32_t primask_val = 1;
    st = core_reg_write(t, REG_PRIMASK, primask_val);
    if (st != NRF_OCD_OK) return st;
    
    /* Resume target to execute the function. */
    st = flash_algo_resume(t);
    if (st != NRF_OCD_OK) return st;
    
    /* Wait for BKPT (core halts). */
    st = flash_algo_wait_halt(t, timeout_ms);
    if (st != NRF_OCD_OK) {
        LOG_DEBUG("Flash algo call timed out (PC=0x%08x)", pc);
        return NRF_OCD_ERR_TIMEOUT;
    }
    
    /* Read R0 for result. */
    nrf_ocd_status_t r = core_reg_read(t, REG_R0, result);
    LOG_DEBUG("flash_algo_call_function result=0x%08X", result ? *result : 0);
    return r;
}

/* Load the flash algorithm into RAM. */
nrf_ocd_status_t flash_algo_load(target_t *t, const flash_algo_t *algo) {
    LOG_DEBUG("Loading flash algo to 0x%08x (%zu words)", algo->load_address, algo->instruction_count);
    nrf_ocd_status_t st = dap_mem_write(&t->dap, 0, algo->load_address,
                         (const void *)algo->instructions,
                         algo->instruction_count * 4);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("flash_algo_load: write failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    /* Verify: read back first 4 words */
    uint32_t verify[4];
    st = dap_mem_read(&t->dap, 0, algo->load_address, (uint8_t *)verify, 16);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("flash_algo_load: verify read failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    LOG_DEBUG("flash_algo_load: verify OK (first word 0x%08X)", verify[0]);
    if (verify[0] != algo->instructions[0]) {
        LOG_ERROR("flash_algo_load: VERIFY FAILED at word 0: got 0x%08X expected 0x%08X",
                  verify[0], algo->instructions[0]);
        return NRF_OCD_ERR_PROTOCOL;
    }
    return NRF_OCD_OK;
}

/* Initialize flash algorithm (call Init function). */
nrf_ocd_status_t flash_algo_init(target_t *t, const flash_algo_t *algo,
                                  uint32_t address, uint32_t clock, uint32_t operation) {
    /* Ensure core is halted. */
    nrf_ocd_status_t st = flash_algo_halt(t);
    if (st != NRF_OCD_OK) return st;

    /* Fresh RRAM chips may need two init attempts (first call times out).
     * Retry once with a warning instead of failing immediately. */
    uint32_t result;
    for (int attempt = 0; attempt < 2; attempt++) {
        st = flash_algo_call_function(t, algo, algo->pc_init, address, clock, operation,
                                       true, 30000, &result);
        if (st == NRF_OCD_OK && result == 0)
            return NRF_OCD_OK;
        if (attempt == 0) {
            LOG_WARNING("Flash algo init timeout (fresh RRAM?), retrying...");
            /* Small delay before retry to let RRAM settle. */
            nrf_ocd_sleep_ms(100);
        }
    }
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash algo init failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    if (result != 0) {
        LOG_ERROR("Flash algo init returned %u", result);
        return NRF_OCD_ERR_FLASH_INIT;
    }
    return NRF_OCD_OK;
}

/* Uninitialize flash algorithm (call UnInit function). */
nrf_ocd_status_t flash_algo_uninit(target_t *t, const flash_algo_t *algo) {
    uint32_t result;
    nrf_ocd_status_t st = flash_algo_call_function(t, algo, algo->pc_uninit, 0, 0, 0,
                                                     false, 30000, &result);
    if (st != NRF_OCD_OK) {
        LOG_WARNING("Flash algo uninit failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    return NRF_OCD_OK;
}

/* Erase one sector via flash algorithm. */
nrf_ocd_status_t flash_algo_erase_sector(target_t *t, const flash_algo_t *algo, uint32_t address) {
    uint32_t result;
    nrf_ocd_status_t st = flash_algo_call_function(t, algo, algo->pc_erase_sector, address, 0, 0,
                                                     false, 30000, &result);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash algo erase sector at 0x%08x failed: %s", address, nrf_ocd_strerror(st));
        return st;
    }
    if (result != 0) {
        LOG_ERROR("Flash algo erase sector returned %u at 0x%08x", result, address);
        return NRF_OCD_ERR_FLASH_ERASE;
    }
    return NRF_OCD_OK;
}

/* Erase all flash via flash algorithm. */
nrf_ocd_status_t flash_algo_erase_all(target_t *t, const flash_algo_t *algo) {
    uint32_t result;
    nrf_ocd_status_t st = flash_algo_call_function(t, algo, algo->pc_erase_all, 0, 0, 0,
                                                     false, 30000, &result);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash algo erase all failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    if (result != 0) {
        LOG_ERROR("Flash algo erase all returned %u", result);
        return NRF_OCD_ERR_FLASH_ERASE;
    }
    return NRF_OCD_OK;
}

/* Program a page of flash via flash algorithm. */
nrf_ocd_status_t flash_algo_program_page(target_t *t, const flash_algo_t *algo,
                                          uint32_t address, const uint8_t *data, size_t len) {
    /* Write data to page buffer in RAM using DAP_TransferBlock.
     * This is ~13x faster than individual word writes. */
    nrf_ocd_status_t st = dap_mem_write(&t->dap, 0, algo->page_buffers[0], data, len);
    if (st != NRF_OCD_OK) return st;
    
    /* Call ProgramPage function. */
    uint32_t result;
    st = flash_algo_call_function(t, algo, algo->pc_program_page, address, (uint32_t)len,
                                   algo->page_buffers[0], false, 30000, &result);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash algo program page failed at 0x%08x: %s", address, nrf_ocd_strerror(st));
        return st;
    }
    if (result != 0) {
        LOG_ERROR("Flash algo program page returned %u at 0x%08x", result, address);
        return NRF_OCD_ERR_FLASH_PROGRAM;
    }
    return NRF_OCD_OK;
}
