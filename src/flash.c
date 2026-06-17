/* flash.c - flash programming engine for nRF54L family.
 *
 * Strategy:
 *   - Chip erase: use the flash algorithm (runs on target CPU).
 *   - Programming: use the flash algorithm to program pages.
 *   - Verify: read back each segment and compare.
 *
 * The flash algorithm is loaded into RAM and executed on the target CPU
 * via core register manipulation (DCRSR/DCRDR), exactly matching pyOCD's
 * approach. Direct AHB writes to flash don't work on nRF54L15.
 */
#include "flash.h"
#include "dap.h"
#include "flash_algo_nrf54l.h"
#include "log.h"
#include "nrf_ocd.h"
#include "target.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

/* Forward declaration. */
static nrf_ocd_status_t verify_segment(target_t *t, const hex_segment_t *seg);

/* Main flash write entry point. */
nrf_ocd_status_t flash_write_image(target_t *t, const hex_image_t *img,
                                   const flash_options_t *opts) {
    if (!t || !img || !opts) return NRF_OCD_ERR_INVALID_ARG;
    if (img->count == 0) {
        LOG_INFO("No segments to program");
        return NRF_OCD_OK;
    }
    
    const flash_algo_t *algo = flash_algo_for_target(t);
    if (!algo) return NRF_OCD_ERR_UNSUPPORTED;

    /* Halt the target before programming. */
    nrf_ocd_status_t st = flash_algo_halt(t);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Failed to halt target: %s", nrf_ocd_strerror(st));
        return st;
    }

    /* Load flash algorithm into RAM. */
    st = flash_algo_load(t, algo);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Failed to load flash algorithm: %s", nrf_ocd_strerror(st));
        return st;
    }

    /* Initialize flash algorithm (must be called BEFORE erase/program). */
    st = flash_algo_init(t, algo, 0, 64000000, 0);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash algo init failed: %s", nrf_ocd_strerror(st));
        return st;
    }

    /* Erase if requested. */
    if (opts->erase != FLASH_ERASE_NONE) {
        LOG_INFO("Erasing chip...");
        st = flash_algo_erase_all(t, algo);
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Erase failed: %s", nrf_ocd_strerror(st));
            flash_algo_uninit(t, algo);
            return st;
        }
        LOG_INFO("Erase complete");
        
        /* Re-halt after erase. */
        st = flash_algo_halt(t);
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Failed to re-halt after erase: %s", nrf_ocd_strerror(st));
            flash_algo_uninit(t, algo);
            return st;
        }
    }

    /* Program each segment. */
    size_t total = 0;
    for (size_t i = 0; i < img->count; i++) total += img->segments[i].size;
    LOG_INFO("Programming %zu byte(s) in %zu segment(s)", total, img->count);
    
    size_t done = 0;
    uint64_t last_log_ms = nrf_ocd_monotonic_ms();
    
    for (size_t i = 0; i < img->count; i++) {
        const hex_segment_t *seg = &img->segments[i];
        
        /* Program in page-sized chunks. */
        size_t off = 0;
        while (off < seg->size) {
            /* Program a single word at a time (flash algorithm page_size=4).
             * Larger chunks cause verify failures because the algorithm only processes
             * min_program_length (4) bytes per call. */
            /* Use 256-byte chunks for good throughput and reliability.
             * The page buffer at 0x20001000 is RAM and can hold any amount of data.
             * The algorithm's program_page function loops processing 'len' bytes. */
            /* Use 4096-byte chunks matching the flash region block size (pyOCD compatible). */
            /* Use 512-byte chunks for good speed and reliability. */
            /* Use 1024-byte chunks for better throughput. */
            /* Use 2048-byte chunks. */
            size_t chunk = 1024;
            if (chunk > seg->size - off) chunk = seg->size - off;
            
            st = flash_algo_program_page(t, algo, seg->address + (uint32_t)off,
                                          seg->data + off, chunk);
            if (st != NRF_OCD_OK) {
                LOG_ERROR("Flash write failed at 0x%08x: %s",
                          seg->address + (uint32_t)off, nrf_ocd_strerror(st));
                flash_algo_uninit(t, algo);
                return st;
            }
            
            off += chunk;
            done += chunk;
            
            uint64_t now = nrf_ocd_monotonic_ms();
            if (now - last_log_ms > 250) {
                printf("  ... %zu / %zu bytes (%.1f%%)\n",
                         done, total, done * 100.0 / (double)total);
                fflush(stdout);
                last_log_ms = now;
            }
        }
        
        /* Verify if requested. */
        if (opts->verify) {
            st = verify_segment(t, seg);
            if (st != NRF_OCD_OK) {
                flash_algo_uninit(t, algo);
                return st;
            }
        }
    }
    
    /* Uninitialize flash algorithm. */
    flash_algo_uninit(t, algo);
    
    LOG_INFO("Programmed %zu bytes in %zu segment(s)", total, img->count);
    return NRF_OCD_OK;
}

/* Verify a segment by reading back in 64-byte chunks. */
static nrf_ocd_status_t verify_segment(target_t *t, const hex_segment_t *seg) {
    size_t off = 0;
    while (off < seg->size) {
        /* Use single-word reads since bulk TransferBlock may timeout. */
        uint32_t expected, actual;
        memcpy(&expected, seg->data + off, 4);
        nrf_ocd_status_t st = target_mem_read_u32(t, seg->address + (uint32_t)off, &actual);
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Verify read failed at 0x%08zx", (size_t)seg->address + off);
            return st;
        }
        if (actual != expected) {
            LOG_ERROR("Verify mismatch at 0x%08zx: got 0x%08x, expected 0x%08x",
                      (size_t)seg->address + off, actual, expected);
            return NRF_OCD_ERR_CRC_MISMATCH;
        }
        off += 4;
    }
    return NRF_OCD_OK;
}

/* Chip erase via CTRL-AP mass erase (for standalone erase command). */
nrf_ocd_status_t flash_chip_erase(target_t *t) {
    if (!t || !t->ops) return NRF_OCD_ERR_INVALID_ARG;
    return t->ops->mass_erase(t);
}
