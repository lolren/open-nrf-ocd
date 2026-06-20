/* flash_algo_nrf54l.h - Flash algorithm for nRF54L family. */
#ifndef FLASH_ALGO_NRF54L_H
#define FLASH_ALGO_NRF54L_H

#include "nrf_ocd.h"
#include "target.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t load_address;
    const uint32_t *instructions;
    size_t instruction_count;
    uint32_t pc_init;
    uint32_t pc_uninit;
    uint32_t pc_program_page;
    uint32_t pc_erase_sector;
    uint32_t pc_erase_all;
    uint32_t static_base;
    uint32_t begin_stack;
    uint32_t page_size;
    uint32_t page_buffers[2];
    uint32_t min_program_length;
    uint32_t flash_start;
    uint32_t flash_size;
} flash_algo_t;

/* Get flash algorithm for target. */
const flash_algo_t *flash_algo_for_target(target_t *t);

/* Load flash algorithm into RAM. */
nrf_ocd_status_t flash_algo_load(target_t *t, const flash_algo_t *algo);

/* Halt/resume the core. */
nrf_ocd_status_t flash_algo_halt(target_t *t);
nrf_ocd_status_t flash_algo_resume(target_t *t);
nrf_ocd_status_t flash_algo_wait_halt(target_t *t, uint32_t timeout_ms);

/* Initialize/uninitialize flash algorithm. */
nrf_ocd_status_t flash_algo_init(target_t *t, const flash_algo_t *algo,
                                  uint32_t address, uint32_t clock, uint32_t operation);
nrf_ocd_status_t flash_algo_uninit(target_t *t, const flash_algo_t *algo);

/* Erase all flash. */
nrf_ocd_status_t flash_algo_erase_all(target_t *t, const flash_algo_t *algo);

/* Program a page. */
nrf_ocd_status_t flash_algo_program_page(target_t *t, const flash_algo_t *algo,
                                          uint32_t address, const uint8_t *data, size_t len);

/* Double-buffered flash programming: start async, wait later. */
nrf_ocd_status_t flash_algo_start_program_page(target_t *t, const flash_algo_t *algo,
                                                uint32_t address, const uint8_t *data, size_t len,
                                                int buffer_num);
nrf_ocd_status_t flash_algo_wait_completion(target_t *t, uint32_t timeout_ms);

/* Sector erase via flash algorithm. */
nrf_ocd_status_t flash_algo_erase_sector(target_t *t, const flash_algo_t *algo, uint32_t address);

#endif /* FLASH_ALGO_NRF54L_H */
