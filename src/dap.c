/* dap.c - DP/AP register and memory accessors. */
#include "dap.h"
#include "log.h"
#include "nrf_ocd.h"
#include "util.h"

#include <string.h>

nrf_ocd_status_t dap_dp_read(cmsis_dap_t *dap, uint8_t addr, uint32_t *value) {
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_DP, addr, true, value);
}

nrf_ocd_status_t dap_dp_write(cmsis_dap_t *dap, uint8_t addr, uint32_t value) {
    /* The DAP_Transfer write command is "fire and forget" - we don't
     * expect a readback, so we use the same cmsis_dap_transfer call with
     * a dummy value. */
    cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_DP, addr, false, &value);
    return NRF_OCD_OK;
}

nrf_ocd_status_t dap_ap_select(cmsis_dap_t *dap, uint8_t ap) {
    return dap_dp_write(dap, DP_SELECT, (uint32_t)ap & 0xFF);
}

nrf_ocd_status_t dap_ap_csw(cmsis_dap_t *dap, uint8_t ap, uint32_t csw) {
    (void)ap;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x0, false, &csw);
}

nrf_ocd_status_t dap_ap_tar(cmsis_dap_t *dap, uint8_t ap, uint32_t tar) {
    (void)ap;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x4, false, &tar);
}

nrf_ocd_status_t dap_ap_read(cmsis_dap_t *dap, uint8_t ap, uint8_t addr, uint32_t *value) {
    nrf_ocd_status_t st = dap_ap_select(dap, ap);
    if (st != NRF_OCD_OK) return st;
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, addr, true, value);
    if (st != NRF_OCD_OK) return st;
    /* Read RDBUFF to flush the pipeline. */
    uint32_t rdbuff;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_DP, DP_RDBUFF, true, &rdbuff);
}

nrf_ocd_status_t dap_ap_write(cmsis_dap_t *dap, uint8_t ap, uint8_t addr, uint32_t value) {
    nrf_ocd_status_t st = dap_ap_select(dap, ap);
    if (st != NRF_OCD_OK) return st;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, addr, false, &value);
}

nrf_ocd_status_t dap_ap_read_block(cmsis_dap_t *dap, uint8_t ap, uint8_t addr,
                                   uint32_t *out, size_t n) {
    nrf_ocd_status_t st = dap_ap_select(dap, ap);
    if (st != NRF_OCD_OK) return st;
    /* Configure CSW: auto-increment + 32-bit + DEVICEEN. */
    /* First issue a single read to set up CSW via the read pathway. */
    uint32_t dummy;
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x0, true, &dummy);
    if (st != NRF_OCD_OK) return st;
    /* Issue the block transfer. */
    st = cmsis_dap_transfer_block(dap, DAP_TRANSFER_APnDP_AP, addr, true,
                                  NULL, out, n);
    if (st != NRF_OCD_OK) return st;
    /* RDBUFF flush. */
    uint32_t rdbuff;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_DP, DP_RDBUFF, true, &rdbuff);
}

nrf_ocd_status_t dap_mem_read(cmsis_dap_t *dap, uint8_t ap, uint32_t addr,
                              void *out, size_t n_bytes) {
    if (n_bytes == 0) return NRF_OCD_OK;
    nrf_ocd_status_t st = dap_ap_select(dap, ap);
    if (st != NRF_OCD_OK) return st;
    /* CSW: word, auto-increment, device-enable. */
    uint32_t csw = CSW_DEVICEEN | CSW_HPROT | CSW_SIZE_WORD | (1U << 4);
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x0, false, &csw);
    if (st != NRF_OCD_OK) return st;
    /* Set TAR (AP register 0x04). */
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x4, false, &addr);
    if (st != NRF_OCD_OK) return st;

    /* Read aligned words via DRW (AP register 0x0C). */
    size_t n_words = n_bytes / 4;
    if (n_words > 0) {
        uint32_t *buf = (uint32_t *)out;
        st = cmsis_dap_transfer_block(dap, DAP_TRANSFER_APnDP_AP, 0xC, true,
                                      NULL, buf, n_words);
        if (st != NRF_OCD_OK) return st;
    }
    size_t tail = n_bytes - n_words * 4;
    if (tail > 0) {
        uint32_t last = 0;
        st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0xC, true, &last);
        if (st != NRF_OCD_OK) return st;
        memcpy((uint8_t *)out + n_words * 4, &last, tail);
    }
    /* RDBUFF flush. */
    uint32_t rdbuff;
    return cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_DP, DP_RDBUFF, true, &rdbuff);
}

nrf_ocd_status_t dap_mem_write(cmsis_dap_t *dap, uint8_t ap, uint32_t addr,
                               const void *in, size_t n_bytes) {
    if (n_bytes == 0) return NRF_OCD_OK;
    nrf_ocd_status_t st = dap_ap_select(dap, ap);
    if (st != NRF_OCD_OK) return st;
    uint32_t csw = CSW_DEVICEEN | CSW_HPROT | CSW_SIZE_WORD | (1U << 4);
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x0, false, &csw);
    if (st != NRF_OCD_OK) return st;
    st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0x4, false, &addr);
    if (st != NRF_OCD_OK) return st;

    const uint8_t *src = (const uint8_t *)in;
    size_t n_words = n_bytes / 4;
    if (n_words > 0) {
        /* Build a local word array because cmsis_dap_transfer_block needs
         * a uint32_t* (and our source is a packed byte stream). */
        uint32_t buf[256];
        if (n_words > 256) {
            /* Bigger than the local buffer; do it in chunks. */
            for (size_t off = 0; off < n_words; off += 256) {
                size_t chunk = n_words - off;
                if (chunk > 256) chunk = 256;
                for (size_t i = 0; i < chunk; i++) {
                    buf[i] = read_le32(src + (off + i) * 4);
                }
                st = cmsis_dap_transfer_block(dap, DAP_TRANSFER_APnDP_AP, 0xC, false,
                                              buf, NULL, chunk);
                if (st != NRF_OCD_OK) return st;
            }
        } else {
            for (size_t i = 0; i < n_words; i++) {
                buf[i] = read_le32(src + i * 4);
            }
            st = cmsis_dap_transfer_block(dap, DAP_TRANSFER_APnDP_AP, 0xC, false,
                                          buf, NULL, n_words);
            if (st != NRF_OCD_OK) return st;
        }
    }
    size_t tail = n_bytes - n_words * 4;
    if (tail > 0) {
        uint32_t last = 0;
        memcpy(&last, src + n_words * 4, tail);
        st = cmsis_dap_transfer(dap, DAP_TRANSFER_APnDP_AP, 0xC, false, &last);
        if (st != NRF_OCD_OK) return st;
    }
    return NRF_OCD_OK;
}
