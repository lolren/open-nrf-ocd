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

enum {
    FLASH_PROGRAM_CHUNK_BYTES = 1024,
};

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
    if (opts->erase == FLASH_ERASE_SECTOR) {
        LOG_ERROR("Sector erase is not implemented for this target; use chip or none");
        return NRF_OCD_ERR_UNSUPPORTED;
    }

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
    if (opts->erase != FLASH_ERASE_NONE && !t->mass_erased) {
        printf("  Erasing chip... ");
        fflush(stdout);
        st = flash_algo_erase_all(t, algo);
        if (st != NRF_OCD_OK) {
            printf("FAILED\n");
            LOG_ERROR("Erase failed: %s", nrf_ocd_strerror(st));
            flash_algo_uninit(t, algo);
            return st;
        }
        printf("done\n");
        t->mass_erased = true;
        
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
    uint64_t t_start = nrf_ocd_monotonic_ms();
    printf("  Programming %zu KB...\n", total / 1024);
    
    size_t done = 0;
    uint64_t last_log_ms = nrf_ocd_monotonic_ms();
    
    for (size_t i = 0; i < img->count; i++) {
        const hex_segment_t *seg = &img->segments[i];
        
        /* Program in chunks accepted reliably by the nRF54L flash algorithm.
         * Larger writes reduce host round-trips; readback verification stays
         * separate so Arduino can choose fast uploads by passing --no-verify. */
        size_t off = 0;
        while (off < seg->size) {
            size_t chunk = FLASH_PROGRAM_CHUNK_BYTES;
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
            if (now - last_log_ms > 200) {
                int _xx = (int)(done * 100 / total);
                int _yy = 30;
                int _zz = (_xx * _yy) / 100;
                int _ww;
                printf("\r  [");
                for (_ww = 0; _ww < _yy; _ww++) printf(_ww < _zz ? "=" : _ww == _zz ? ">" : " ");
                printf("] %3d%% (%zu/%zu KB)", _xx, done / 1024, total / 1024);
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
    
    uint64_t elapsed = (nrf_ocd_monotonic_ms() - t_start) / 1000;
    /* progress bar already shown above */
    printf("  Done: %zu bytes in %lu s (%.1f kB/s)\n", total, (unsigned long)elapsed, 
           elapsed > 0 ? (double)total / (1024.0 * (double)elapsed) : 0.0);
    return NRF_OCD_OK;
}

/* Verify a segment with conservative word reads. Some Seeed CMSIS-DAP bridge
 * paths timeout on AP memory TransferBlock reads, so keep readback robust here
 * and let callers opt out with --no-verify when upload speed matters. */
static nrf_ocd_status_t verify_segment(target_t *t, const hex_segment_t *seg) {
    for (size_t off = 0; off < seg->size; off += 4) {
        uint8_t expected_bytes[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        uint32_t actual = 0;
        size_t tail = seg->size - off;
        if (tail > sizeof(expected_bytes)) tail = sizeof(expected_bytes);
        memcpy(expected_bytes, seg->data + off, tail);
        nrf_ocd_status_t st = target_mem_read_u32(t, seg->address + (uint32_t)off, &actual);
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Verify read failed at 0x%08zx", (size_t)seg->address + off);
            return st;
        }
        uint8_t actual_bytes[4];
        memcpy(actual_bytes, &actual, sizeof(actual_bytes));
        if (memcmp(actual_bytes, expected_bytes, tail) != 0) {
            uint32_t expected = 0;
            memcpy(&expected, expected_bytes, sizeof(expected));
            LOG_ERROR("Verify mismatch at 0x%08zx: got 0x%08x, expected 0x%08x",
                      (size_t)seg->address + off, actual, expected);
            return NRF_OCD_ERR_CRC_MISMATCH;
        }
    }
    return NRF_OCD_OK;
}

/* Chip erase via CTRL-AP mass erase (for standalone erase command). */
nrf_ocd_status_t flash_chip_erase(target_t *t) {
    if (!t || !t->ops) return NRF_OCD_ERR_INVALID_ARG;
    return t->ops->mass_erase(t);
}
