/* swd.c - SWD line-level sequencing. */
#include "swd.h"
#include "log.h"
#include "util.h"

#include <string.h>

/* JTAG-to-SWD select sequence (16 bits, LSB first): the canonical
 * CoreSight deprecated ADIv5 sequence 0xE79E. The DAP_SWJ_Sequence
 * command sends bits LSB first within each byte, so the value
 * 0xE79E encodes to byte 0 = 0x9E, byte 1 = 0xE7. */
static const uint8_t SWD_SELECT_SEQ[2] = { 0x9E, 0xE7 };

nrf_ocd_status_t swd_switch_to_swd(cmsis_dap_t *dap) {
    /* Matches pyOCD's deprecated ADIv5 SWJ switch_to_swd() sequence exactly,
     * observed on the XIAO nRF54 (all via DAP_SWJ_Sequence 0x12):
     *   1. line reset:  >=50 SWDIO high  (51 bits)
     *   2. JTAG-to-SWD select: 16 bits 0xE79E (LSB-first = 9E E7)
     *   3. line reset:  51 bits high
     *   4. idle:        8 bits low
     * Without the select sequence the DP never leaves JTAG/dormant and the
     * first SWD transfer returns NO_ACK. */
    static const uint8_t line_reset[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t idle[1] = { 0x00 };
    nrf_ocd_status_t st;
    st = cmsis_dap_swj_sequence(dap, line_reset, 51);
    if (st != NRF_OCD_OK) return st;
    st = cmsis_dap_swj_sequence(dap, SWD_SELECT_SEQ, 16);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("SWD select sequence failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    st = cmsis_dap_swj_sequence(dap, line_reset, 51);
    if (st != NRF_OCD_OK) return st;
    return cmsis_dap_swj_sequence(dap, idle, 8);
}

nrf_ocd_status_t swd_line_reset(cmsis_dap_t *dap) {
    /* pyOCD uses 51 high bits for a line reset. */
    static const uint8_t line_reset[7] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    return cmsis_dap_swj_sequence(dap, line_reset, 51);
}

nrf_ocd_status_t swd_line_reset_swd_sequence(cmsis_dap_t *dap) {
    /* Uses DAP_SWD_SEQUENCE (0x1D) which is more reliable on
     * the XIAO SAMD11 bridge. Sends 51 high bits then 8 idle. */
    uint8_t seq_info[2] = { 51, 8 };  /* 51 high, 8 idle */
    uint8_t seq_data[2][8] = {
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
    };
    return cmsis_dap_swd_sequence(dap, (uint8_t *)seq_info, (uint8_t *)seq_data, 2);
}

nrf_ocd_status_t swd_read_idcode(cmsis_dap_t *dap, uint32_t *idcode) {
    if (!idcode) return NRF_OCD_ERR_INVALID_ARG;
    /* pyOCD reads the DP IDR via DAP_TransferBlock (0x06); on the XIAO
     * SAMD11 bridge a single DAP_Transfer (0x05) of the IDCODE returns
     * NO_ACK, while the block form succeeds. */
    return cmsis_dap_transfer_block(dap, DAP_TRANSFER_APnDP_DP, 0x0, true,
                                    NULL, idcode, 1);
}
