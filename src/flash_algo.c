/*
 * flash_algo.c - nRF54 flash algorithm implementation
 *
 * Implements flash programming by loading the position-independent flash
 * algorithm into target RAM and invoking it via Cortex-M register writes.
 * Based on the flash algorithm from pyocd's target_nRF54L15.py.
 */

#include "nrf_ocd.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ==================== Flash Algorithm Instructions ==================== */
/*
 * Position-independent Thumb-2 code for nRF54 flash programming.
 * This is the actual ARM assembly from Nordic's flash algorithm,
 * converted to 32-bit instruction words.
 *
 * Functions:
 *   Init(r0=addr, r1=clock, r2=operation) -> r0=0 on success
 *   UnInit(r0=operation) -> r0=0 on success
 *   EraseAll() -> r0=0 on success
 *   EraseSector(r0=sector_addr) -> r0=0 on success
 *   ProgramPage(r0=flash_addr, r1=num_bytes, r2=data_ptr) -> r0=0 on success
 *
 * Static data at static_base:
 *   [0x00] = NVMC_BASE address
 *   [0x04] = NVMC_CONFIG register value
 *   ...
 */

/* Flash algorithm instruction words (from pyOCD target_nRF54L15.py) */
static const uint32_t nrf54l15_flash_algo_instructions[] = {
    0xE00ABE00,
    0xf8d24a02, 0x2b013400, 0x4770d1fb, 0x5004b000, 0x47702000,
    0x47702000, 0x49072001, 0xf8c1b508, 0xf7ff0500,
    0xf8c1ffed, 0x20000540, 0xffe8f7ff, 0x0500f8c1,
    0xbf00bd08, 0x5004b000, 0x2301b508, 0xf8c14906,
    0xf7ff3500, 0xf04fffdb, 0x600333ff, 0xf7ff2000,
    0xf8c1ffd5, 0xbd080500, 0x5004b000, 0x2301b538,
    0x4d0c4614, 0x0103f021, 0x3500f8c5, 0xffc6f7ff,
    0x44214622, 0x42911b00, 0x2000d105, 0xffbef7ff,
    0x0500f8c5, 0x4613bd38, 0x4b04f853, 0x461a5014,
    0xbf00e7f1, 0x5004b000, 0x00000000
};

/* Flash algorithm instruction words (from pyOCD target_nRF54LM20A.py — SAME as L15). */
static const uint32_t nrf54lm20a_flash_algo_instructions[] = {
    0xE00ABE00,
    0xf8d24a02, 0x2b013400, 0x4770d1fb, 0x5004E000, 0x47702000,
    0x47702000, 0x49072001, 0xf8c1b508,
    0xf7ff0500, 0xf8c1ffed, 0x20000540, 0xffe8f7ff, 0x0500f8c1,
    0xbf00bd08, 0x5004E000, 0x2301b508,
    0xf8c14906, 0xf7ff3500, 0xf04fffdb, 0x600333ff, 0xf7ff2000,
    0xf8c1ffd5, 0xbd080500, 0x5004E000,
    0x2301b538, 0x4d0c4614, 0x0103f021, 0x3500f8c5, 0xffc6f7ff,
    0x44214622, 0x42911b00, 0x2000d105,
    0xffbef7ff, 0x0500f8c5, 0x4613bd38, 0x4b04f853, 0x461a5014,
    0xbf00e7f1, 0x5004E000, 0x00000000
};

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const nrf_target_desc_t nrf54l15_target = {
    .name = "nrf54l15",
    .flash_size = NRF54L15_FLASH_SIZE,
    .ram_size = 0x40000,
    .flash_algo = {
        .instructions = nrf54l15_flash_algo_instructions,
        .instruction_count = ARRAY_COUNT(nrf54l15_flash_algo_instructions),
        .load_address = NRF54_ALGO_LOAD_ADDR,
        .pc_init = NRF54_ALGO_INIT,
        .pc_uninit = NRF54_ALGO_UNINIT,
        .pc_erase_all = NRF54_ALGO_ERASE_ALL,
        .pc_erase_sector = NRF54_ALGO_ERASE_SECTOR,
        .pc_program_page = NRF54_ALGO_PROGRAM_PAGE,
        .static_base = NRF54_ALGO_STATIC_BASE,
        .stack_top = NRF54_ALGO_STACK_TOP,
        .page_buffers = { 0x20001000, 0x20001004 },
        .page_size = 4,
        .min_program_length = NRF54_MIN_PROG_LEN,
    },
};

const nrf_target_desc_t nrf54lm20a_target = {
    .name = "nrf54lm20a",
    .flash_size = NRF54LM20A_FLASH_SIZE,
    .ram_size = 0x80000,
    .flash_algo = {
        .instructions = nrf54lm20a_flash_algo_instructions,
        .instruction_count = ARRAY_COUNT(nrf54lm20a_flash_algo_instructions),
        .load_address = NRF54_ALGO_LOAD_ADDR,
        .pc_init = NRF54_ALGO_INIT,
        .pc_uninit = NRF54_ALGO_UNINIT,
        .pc_erase_all = NRF54_ALGO_ERASE_ALL,
        .pc_erase_sector = NRF54_ALGO_ERASE_SECTOR,
        .pc_program_page = NRF54_ALGO_PROGRAM_PAGE,
        .static_base = NRF54_ALGO_STATIC_BASE,
        .stack_top = NRF54_ALGO_STACK_TOP,
        .page_buffers = { 0x20001000, 0x20001004 },
        .page_size = 4,
        .min_program_length = NRF54_MIN_PROG_LEN,
    },
};

/* ==================== Cortex-M Register Access ==================== */

/* Debug Halting Control and Status Register */
#define DHCSR     0xE000EDF0
#define DHCSR_KEY 0xA05F0000
#define DHCSR_C_DEBUGEN (1 << 0)
#define DHCSR_C_HALT (1 << 1)
#define DHCSR_S_REGRDY (1 << 16)
#define DHCSR_S_HALT (1 << 17)

/* Debug Core Register Selector Register */
#define DCRSR     0xE000EDF4
#define DCRSR_REGW (1 << 16)

/* Debug Core Register Data Register */
#define DCRDR     0xE000EDF8

/* Application Interrupt and Reset Control Register */
#define AIRCR     0xE000ED0C
#define AIRCR_VECTKEY 0x05FA0000
#define AIRCR_VECTRESET (1 << 0)

/* Core register numbers (for DCRSR) */
#define REG_R0    0
#define REG_R1    1
#define REG_R2    2
#define REG_R3    3
#define REG_R9    9
#define REG_R12   12
#define REG_R13   13  /* SP */
#define REG_R14   14  /* LR */
#define REG_R15   15  /* PC */
#define REG_XPSR  16

static void sleep_us(uint32_t usec) {
    while (usec > 0) {
        uint32_t chunk = usec > 1000000U ? 1000000U : usec;
        usleep(chunk);
        usec -= chunk;
    }
}

static nrf_ocd_error_t cortex_m_wait_reg_ready(nrf_ap_t *ap) {
    for (int i = 0; i < 1000; i++) {
        uint32_t dhcsr;
        nrf_ocd_error_t err = nrf_mem_read32(ap, DHCSR, &dhcsr);
        if (err != NRF_OCD_OK)
            return err;
        if (dhcsr & DHCSR_S_REGRDY)
            return NRF_OCD_OK;
        sleep_us(1000);
    }

    return NRF_OCD_ERR_TIMEOUT;
}

static nrf_ocd_error_t cortex_m_write_reg(nrf_ap_t *ap, int reg_num, uint32_t value) {
    nrf_ocd_error_t err = cortex_m_wait_reg_ready(ap);
    if (err != NRF_OCD_OK)
        return err;

    err = nrf_mem_write32(ap, DCRDR, value);
    if (err != NRF_OCD_OK)
        return err;

    err = nrf_mem_write32(ap, DCRSR, DCRSR_REGW | (uint32_t)reg_num);
    if (err != NRF_OCD_OK)
        return err;

    return cortex_m_wait_reg_ready(ap);
}

static nrf_ocd_error_t cortex_m_read_reg(nrf_ap_t *ap, int reg_num, uint32_t *value) {
    nrf_ocd_error_t err = cortex_m_wait_reg_ready(ap);
    if (err != NRF_OCD_OK)
        return err;

    err = nrf_mem_write32(ap, DCRSR, (uint32_t)reg_num);
    if (err != NRF_OCD_OK)
        return err;

    err = cortex_m_wait_reg_ready(ap);
    if (err != NRF_OCD_OK)
        return err;
    return nrf_mem_read32(ap, DCRDR, value);
}

static nrf_ocd_error_t cortex_m_write_regs(nrf_ap_t *ap, const int *regs, const uint32_t *values, int count) {
    for (int i = 0; i < count; i++) {
        nrf_ocd_error_t err = cortex_m_write_reg(ap, regs[i], values[i]);
        if (err != NRF_OCD_OK)
            return err;
    }
    return NRF_OCD_OK;
}

static nrf_ocd_error_t cortex_m_halt(nrf_ap_t *ap) {
    nrf_ocd_error_t err = nrf_mem_write32(ap, DHCSR,
                                          DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
    if (err != NRF_OCD_OK)
        return err;

    for (int i = 0; i < 1000; i++) {
        uint32_t dhcsr;
        err = nrf_mem_read32(ap, DHCSR, &dhcsr);
        if (err != NRF_OCD_OK)
            return err;
        if (dhcsr & DHCSR_S_HALT)
            return NRF_OCD_OK;
        sleep_us(1000);
    }

    return NRF_OCD_ERR_TIMEOUT;
}

static nrf_ocd_error_t cortex_m_resume(nrf_ap_t *ap) {
    return nrf_mem_write32(ap, DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN);
}

static nrf_ocd_error_t cortex_m_wait_halted(nrf_ap_t *ap, uint32_t timeout_ms) {
    if (timeout_ms == 0)
        timeout_ms = 1;

    for (uint32_t elapsed = 0; elapsed < timeout_ms; elapsed++) {
        uint32_t dhcsr;
        nrf_ocd_error_t err = nrf_mem_read32(ap, DHCSR, &dhcsr);
        if (err != NRF_OCD_OK)
            return err;
        if (dhcsr & DHCSR_S_HALT)
            return NRF_OCD_OK;
        sleep_us(1000);
    }

    return NRF_OCD_ERR_TIMEOUT;
}

/* ==================== Flash Algorithm Execution ==================== */

/*
 * Call a function in the flash algorithm and wait for it to complete.
 * The function returns to a BKPT instruction at load_address, halting the core.
 *
 * @param flash  Flash context
 * @param pc     Program counter (entry point, must be Thumb - bit 0 set)
 * @param r0     R0 argument
 * @param r1     R1 argument
 * @param r2     R2 argument
 * @param r3     R3 argument
 * @param init   If true, also set R9=static_base, SP=stack_top, XPSR=Thumb
 * @param timeout_ms  Timeout in milliseconds
 * @param out_r0  Output: R0 return value
 * @return NRF_OCD_OK on success, error code on failure
 */
static nrf_ocd_error_t call_flash_function(nrf_flash_t *flash,
                                           uint32_t pc,
                                           uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                                           bool init,
                                           uint32_t timeout_ms,
                                           uint32_t *out_r0) {
    nrf_ap_t *ap = flash->ap;
    const nrf_flash_algo_t *algo = &flash->target->flash_algo;
    int regs[10];
    uint32_t values[10];
    int count = 0;
    nrf_ocd_error_t err;

    err = cortex_m_halt(ap);
    if (err != NRF_OCD_OK)
        return err;

    regs[count] = REG_R15; values[count] = pc | 1U; count++;
    regs[count] = REG_R0; values[count] = r0; count++;
    regs[count] = REG_R1; values[count] = r1; count++;
    regs[count] = REG_R2; values[count] = r2; count++;
    regs[count] = REG_R3; values[count] = r3; count++;

    if (init) {
        regs[count] = REG_R9;  values[count] = algo->static_base; count++;
        regs[count] = REG_R13; values[count] = algo->stack_top; count++;
        regs[count] = REG_XPSR; values[count] = 0x01000000; count++;
    }

    regs[count] = REG_R14; values[count] = algo->load_address | 1U; count++;

    err = cortex_m_write_regs(ap, regs, values, count);
    if (err != NRF_OCD_OK)
        return err;

    err = cortex_m_resume(ap);
    if (err != NRF_OCD_OK)
        return err;

    err = cortex_m_wait_halted(ap, timeout_ms);
    if (err != NRF_OCD_OK) {
        /* One last check — the core may have halted just after our last poll */
        uint32_t dhcsr;
        if (nrf_mem_read32(ap, DHCSR, &dhcsr) == NRF_OCD_OK && (dhcsr & DHCSR_S_HALT)) {
            NRF_DBG("Core halted on final check after timeout");
        } else {
            cortex_m_halt(ap);
            NRF_ERR("Flash function call timed out");
            return err;
        }
    }

    uint32_t ret;
    err = cortex_m_read_reg(ap, REG_R0, &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (out_r0)
        *out_r0 = ret;

    return NRF_OCD_OK;
}

/* ==================== Flash API ==================== */

nrf_ocd_error_t nrf_flash_prepare(nrf_flash_t *flash) {
    nrf_ap_t *ap = flash->ap;
    if (!flash->target)
        return NRF_OCD_ERR_FLASH_INIT;

    const nrf_flash_algo_t *algo = &flash->target->flash_algo;

    /* Halt the core first so we can write to RAM. The core must be
     * halted for memory access via the debugger. */
    nrf_ocd_error_t err = cortex_m_halt(ap);
    if (err != NRF_OCD_OK) {
        NRF_ERR("Failed to halt core: %s", nrf_ocd_error_str(err));
        return err;
    }

    /* Load flash algorithm instructions into RAM */
    NRF_INFO("Loading %s flash algorithm to 0x%08X", flash->target->name, algo->load_address);

    err = nrf_mem_write_block32(ap, algo->load_address,
                                 algo->instructions,
                                 algo->instruction_count);
    if (err != NRF_OCD_OK) {
        NRF_ERR("Failed to write flash algorithm to RAM: %s", nrf_ocd_error_str(err));
        return err;
    }

    flash->prepared = true;
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_init(nrf_flash_t *flash, int operation, uint32_t addr, uint32_t clock) {
    if (!flash->prepared) {
        nrf_ocd_error_t err = nrf_flash_prepare(flash);
        if (err != NRF_OCD_OK)
            return err;
    }

    if (flash->inited && flash->operation == operation)
        return NRF_OCD_OK;

    if (flash->inited) {
        nrf_ocd_error_t err = nrf_flash_uninit(flash);
        if (err != NRF_OCD_OK)
            return err;
    }

    /* Call Init(r0=addr, r1=clock, r2=operation) */
    uint32_t ret;
    nrf_ocd_error_t err = call_flash_function(flash,
                                              flash->target->flash_algo.pc_init,
                                              addr, clock, (uint32_t)operation, 0,
                                              true,  /* init */
                                              15000, /* 15s timeout */
                                              &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (ret != 0) {
        NRF_ERR("Flash Init returned error code 0x%08X", ret);
        return NRF_OCD_ERR_FLASH_INIT;
    }

    flash->inited = true;
    flash->operation = operation;
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_uninit(nrf_flash_t *flash) {
    if (!flash->inited)
        return NRF_OCD_OK;

    uint32_t ret;
    nrf_ocd_error_t err = call_flash_function(flash,
                                              flash->target->flash_algo.pc_uninit,
                                              (uint32_t)flash->operation, 0, 0, 0,
                                              false,
                                              15000,
                                              &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (ret != 0) {
        NRF_WARN("Flash UnInit returned 0x%08X", ret);
    }

    flash->inited = false;
    flash->operation = 0;
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_erase_all(nrf_flash_t *flash) {
    if (!flash->inited || flash->operation != 1) {
        nrf_ocd_error_t err = nrf_flash_init(flash, 1, 0, 0);
        if (err != NRF_OCD_OK)
            return err;
    }

    NRF_INFO("Erasing all flash...");

    uint32_t ret;
    nrf_ocd_error_t err = call_flash_function(flash,
                                              flash->target->flash_algo.pc_erase_all,
                                              0, 0, 0, 0,
                                              false,
                                              120000, /* up to 2min for large flash */
                                              &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (ret != 0) {
        NRF_ERR("Flash EraseAll returned error code 0x%08X", ret);
        return NRF_OCD_ERR_FLASH_ERASE;
    }

    NRF_INFO("Flash erase complete");
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_erase_sector(nrf_flash_t *flash, uint32_t addr) {
    if (!flash->inited || flash->operation != 1) {
        nrf_ocd_error_t err = nrf_flash_init(flash, 1, 0, 0);
        if (err != NRF_OCD_OK)
            return err;
    }

    NRF_INFO("Erasing sector at 0x%08X", addr);

    uint32_t ret;
    nrf_ocd_error_t err = call_flash_function(flash,
                                              flash->target->flash_algo.pc_erase_sector,
                                              addr, 0, 0, 0,
                                              false,
                                              60000,
                                              &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (ret != 0) {
        NRF_ERR("Flash EraseSector returned error code 0x%08X", ret);
        return NRF_OCD_ERR_FLASH_ERASE;
    }

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_program_page(nrf_flash_t *flash, uint32_t addr, const uint8_t *data, int len) {
    const nrf_flash_algo_t *algo = &flash->target->flash_algo;

    if (len <= 0)
        return NRF_OCD_OK;
    if ((addr % algo->min_program_length) != 0 || (len % (int)algo->min_program_length) != 0) {
        NRF_ERR("Program request must be %u-byte aligned (addr=0x%08X len=%d)",
                algo->min_program_length, addr, len);
        return NRF_OCD_ERR_FLASH_PROGRAM;
    }

    if (!flash->inited || flash->operation != 2) {
        nrf_ocd_error_t err = nrf_flash_init(flash, 2, 0, 0);
        if (err != NRF_OCD_OK)
            return err;
    }

    /* Convert byte data to 32-bit words for TransferBlock write.
     * Use a temporary aligned buffer to avoid unaligned access UB. */
    int word_count = len / 4;
    uint32_t word_buf[256];  /* max 256 words = 1024 bytes */
    uint32_t *words = word_buf;
    int on_stack = 1;
    nrf_ocd_error_t err;

    if (word_count > 256) {
        words = malloc((size_t)word_count * 4);
        if (!words)
            return NRF_OCD_ERR_MEMORY;
        on_stack = 0;
    }
    for (int i = 0; i < word_count; i++) {
        memcpy(&words[i], data + i * 4, 4);
    }
    err = nrf_mem_write_block32(flash->ap, algo->page_buffers[0],
                                 words, word_count);
    if (!on_stack)
        free(words);
    if (err != NRF_OCD_OK)
        return err;

    /* Call ProgramPage(r0=flash_addr, r1=num_bytes, r2=data_ptr) */
    uint32_t ret;
    err = call_flash_function(flash,
                              algo->pc_program_page,
                              addr, (uint32_t)len, algo->page_buffers[0], 0,
                              false,
                              60000,  /* 60s timeout for program */
                              &ret);
    if (err != NRF_OCD_OK)
        return err;

    if (ret != 0) {
        NRF_ERR("Flash ProgramPage at 0x%08X returned error 0x%08X", addr, ret);
        return NRF_OCD_ERR_FLASH_PROGRAM;
    }

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_flash_cleanup(nrf_flash_t *flash) {
    nrf_flash_uninit(flash);
    flash->prepared = false;
    flash->inited = false;
    flash->operation = 0;
    return NRF_OCD_OK;
}
