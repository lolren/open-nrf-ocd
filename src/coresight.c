/*
 * coresight.c - CoreSight SWD connect, DP/AP access, nRF54 mass erase
 */

#include "nrf_ocd.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void sleep_ms(unsigned ms) {
    while (ms > 0) {
        unsigned chunk = ms > 1000U ? 1000U : ms;
        usleep(chunk * 1000U);
        ms -= chunk;
    }
}

static nrf_ocd_error_t dap_status_command(nrf_dap_t *dap, const uint8_t *cmd, int cmd_len) {
    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = nrf_probe_write(dap->probe, cmd, cmd_len);
    if (err != NRF_OCD_OK)
        return err;

    err = nrf_probe_read(dap->probe, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    if (resp_len < 2 || resp[0] != cmd[0] || resp[1] != 0)
        return NRF_OCD_ERR_DAP_CMD;

    return NRF_OCD_OK;
}

/* Internal: perform an SWD line reset using SWD_SEQUENCE.
 * Sends >50 SWCLK cycles with SWDIO high, then 8+ idle cycles. */
static nrf_ocd_error_t swd_line_reset(nrf_dap_t *dap) {
    /* Sequence 1: 51 cycles with SWDIO=high (LSB-first, all ones). */
    uint8_t seq_info[2];
    uint8_t seq_data[2][8];
    uint8_t out_data[2][8];

    seq_info[0] = 51 & 0x3F;   /* output, 51 cycles */
    memset(seq_data[0], 0xFF, sizeof(seq_data[0])); /* all ones */

    seq_info[1] = 8 & 0x3F;    /* output, 8 idle cycles */
    memset(seq_data[1], 0x00, sizeof(seq_data[1])); /* all zeros */

    nrf_ocd_error_t err = nrf_dap_swd_sequence(dap, 2, seq_info,
                                                  (const uint8_t (*)[8])seq_data, out_data);
    if (err != NRF_OCD_OK) {
        NRF_DBG("SWD line reset: sequence failed (%s)", nrf_ocd_error_str(err));
        return err;
    }

    /* Read DP IDCODE to exit line-reset state and confirm the target is alive. */
    uint32_t idr;
    err = nrf_dp_read(dap, DP_IDCODE, &idr);
    if (err != NRF_OCD_OK) {
        NRF_DBG("SWD line reset: DP IDCODE read failed (%s)", nrf_ocd_error_str(err));
        return err;
    }

    NRF_DBG("SWD line reset: DP IDCODE = 0x%08X", idr);
    return NRF_OCD_OK;
}

static void swd_sleep_ms(int ms) {
    while (ms-- > 0) sleep_ms(1);
}

nrf_ocd_error_t nrf_swd_connect(nrf_dap_t *dap) {
    nrf_ocd_error_t err;
    uint32_t ctrl_stat;
    bool powered;
    int attempt;

    for (attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) {
            NRF_WARN("SWD connect attempt %d/5...", attempt + 1);
            swd_sleep_ms(50 * (1 << attempt));
        }

        /* Step 1: DAP_Connect — enter SWD mode. */
        err = nrf_dap_connect(dap, 1);
        if (err != NRF_OCD_OK) {
            NRF_DBG("DAP_Connect failed on attempt %d: %s", attempt + 1, nrf_ocd_error_str(err));
            continue;
        }

        /* Step 2: Configure SWD turnaround timing (matches pyOCD). */
        {
            uint8_t cmd[2] = { 0x13, 0x00 };
            err = dap_status_command(dap, cmd, sizeof(cmd));
            if (err != NRF_OCD_OK) {
                NRF_DBG("SWD configure failed on attempt %d", attempt + 1);
                continue;
            }
        }

        /* Step 3: Configure transfer parameters (idle=2, wait_retry=150, match=0). */
        {
            uint8_t tcmd[7] = { 0x04, 2, 0x96, 0x00, 0x00, 0x00, 0x00 };
            err = dap_status_command(dap, tcmd, sizeof(tcmd));
            if (err != NRF_OCD_OK) {
                NRF_DBG("Transfer configure failed on attempt %d", attempt + 1);
                continue;
            }
        }

        /* Step 4: SWD line reset — >50 cycles SWDIO high, 8 idle,
         * then read DP IDCODE to exit reset. */
        err = swd_line_reset(dap);
        if (err != NRF_OCD_OK) {
            NRF_DBG("SWD line reset failed on attempt %d: %s", attempt + 1, nrf_ocd_error_str(err));
            continue;
        }

        /* Step 5: Switch to DP bank 0 and clear sticky errors. */
        err = nrf_dp_write(dap, DP_SELECT, 0x00000000);
        if (err != NRF_OCD_OK) {
            NRF_DBG("DP_SELECT write failed on attempt %d", attempt + 1);
            continue;
        }
        err = nrf_dp_write(dap, DP_ABORT,
            DP_ABORT_ORUNERRCLR | DP_ABORT_WDERRCLR |
            DP_ABORT_STKERRCLR | DP_ABORT_STKCMPCLR);
        if (err != NRF_OCD_OK) {
            NRF_DBG("Pre-powerup DP_ABORT failed on attempt %d", attempt + 1);
            continue;
        }

        /* Step 6: Request debug and system power (without MASKLANE).
         * This matches pyOCD's DebugPortStart hook value. */
        err = nrf_dp_write(dap, DP_CTRL_STAT, DP_CTRL_CSYSPWRUPREQ | DP_CTRL_CDBGPWRUPREQ);
        if (err != NRF_OCD_OK) {
            NRF_DBG("DP_CTRL_STAT power-up write failed on attempt %d", attempt + 1);
            continue;
        }

        /* Step 7: Wait for both power domains to acknowledge. */
        powered = false;
        for (int i = 0; i < 500; i++) {
            err = nrf_dp_read(dap, DP_CTRL_STAT, &ctrl_stat);
            if (err != NRF_OCD_OK) break;
            if ((ctrl_stat & (DP_CTRL_CSYSPWRUPACK | DP_CTRL_CDBGPWRUPACK)) ==
                (DP_CTRL_CSYSPWRUPACK | DP_CTRL_CDBGPWRUPACK)) {
                powered = true;
                break;
            }
            sleep_ms(10);
        }

        if (!powered) {
            NRF_WARN("Debug power-up not acknowledged on attempt %d (CTRL_STAT=0x%08X)",
                     attempt + 1, ctrl_stat);
            continue;
        }

        /* Step 8: Clear sticky errors after power-up. */
        err = nrf_dp_write(dap, DP_ABORT,
            DP_ABORT_ORUNERRCLR | DP_ABORT_WDERRCLR |
            DP_ABORT_STKERRCLR | DP_ABORT_STKCMPCLR);
        if (err != NRF_OCD_OK) {
            NRF_DBG("DP_ABORT write failed on attempt %d", attempt + 1);
            continue;
        }

        NRF_INFO("SWD connected successfully on attempt %d", attempt + 1);
        return NRF_OCD_OK;
    }

    NRF_ERR("SWD connect failed after %d attempts", attempt);
    return NRF_OCD_ERR_SWD_CONNECT;
}

nrf_ocd_error_t nrf_swd_disconnect(nrf_dap_t *dap) {
    return nrf_dap_disconnect(dap);
}

nrf_ocd_error_t nrf54_ctrl_mass_erase(nrf_dap_t *dap) {
    uint32_t ctrl_base = (uint32_t)2 << 24;
    uint32_t idr;

    /* Invalidate DP_SELECT cache so AP#2 will be selected fresh.
     * The DP may be in a bad state from a failed SWD connect; this
     * ensures we don't reuse a stale cached value. */
    dap->select_valid = false;

    /* Try to power up the DP. Ignore errors — on APPROTECT-locked
     * nRF54, the DP may reject power-up writes but still allow
     * CTRL-AP access. */
    nrf_dp_write(dap, DP_CTRL_STAT, DP_CTRL_CSYSPWRUPREQ | DP_CTRL_CDBGPWRUPREQ);
    /* Also clear any sticky errors */
    nrf_dap_write_abort(dap, DP_ABORT_DAPABORT | DP_ABORT_STKCMPCLR |
                        DP_ABORT_STKERRCLR | DP_ABORT_WDERRCLR |
                        DP_ABORT_ORUNERRCLR);

    nrf_ocd_error_t err = nrf_ap_read(dap, ctrl_base | CTRL_AP_IDR, &idr);
    if (err != NRF_OCD_OK) {
        NRF_DBG("CTRL-AP IDR read failed: %s", nrf_ocd_error_str(err));
        return err;
    }

    if (idr != CTRL_AP_IDR_EXPECTED) {
        NRF_ERR("CTRL-AP IDR mismatch: expected 0x%08X, got 0x%08X", CTRL_AP_IDR_EXPECTED, idr);
        return NRF_OCD_ERR_SWD_CONNECT;
    }

    err = nrf_ap_write(dap, ctrl_base | CTRL_AP_ERASEALL, 1);
    if (err != NRF_OCD_OK) {
        NRF_WARN("CTRL-AP mass erase write failed");
        return err;
    }

    bool saw_busy = false;
    for (int i = 0; i < 300; i++) {
        uint32_t status;
        err = nrf_ap_read(dap, ctrl_base | CTRL_AP_ERASEALLSTATUS, &status);
        if (err != NRF_OCD_OK)
            return err;

        if (status == CTRL_AP_ERASEALLSTATUS_BUSY) {
            saw_busy = true;
            break;
        }
        if (status == CTRL_AP_ERASEALLSTATUS_ERROR) {
            NRF_ERR("CTRL-AP mass erase error");
            return NRF_OCD_ERR_FLASH_ERASE;
        }

        sleep_ms(100);
    }

    if (!saw_busy) {
        NRF_ERR("CTRL-AP mass erase did not start");
        return NRF_OCD_ERR_TIMEOUT;
    }

    bool ready_to_reset = false;
    for (int i = 0; i < 300; i++) {
        uint32_t status;
        err = nrf_ap_read(dap, ctrl_base | CTRL_AP_ERASEALLSTATUS, &status);
        if (err != NRF_OCD_OK)
            return err;

        if (status == CTRL_AP_ERASEALLSTATUS_READYTORESET) {
            ready_to_reset = true;
            break;
        }
        if (status == CTRL_AP_ERASEALLSTATUS_ERROR) {
            NRF_ERR("CTRL-AP mass erase error");
            return NRF_OCD_ERR_FLASH_ERASE;
        }

        sleep_ms(100);
    }

    if (!ready_to_reset) {
        NRF_ERR("CTRL-AP mass erase timed out waiting for reset-ready status");
        return NRF_OCD_ERR_TIMEOUT;
    }

    sleep_ms(10);
    err = nrf_ap_write(dap, ctrl_base | CTRL_AP_RESET, 2);
    if (err != NRF_OCD_OK) return err;
    err = nrf_ap_write(dap, ctrl_base | CTRL_AP_RESET, 0);
    if (err != NRF_OCD_OK) return err;
    sleep_ms(200);

    return NRF_OCD_OK;
}

/* ==================== MEM-AP access ==================== */

/* DAP transfer access type bits (mirrored from cmsis_dap.c internals) */
#define DP_ACC_BIT  0x00
#define AP_ACC_BIT  0x01
#define READ_BIT    0x02
#define WRITE_BIT   0x00

static const uint32_t MEM_AP_CSW = 0x00;
static const uint32_t MEM_AP_TAR = 0x04;
static const uint32_t MEM_AP_DRW = 0x0C;

/* Base CSW read from the MEM-AP during init. pyOCD reads the
 * original CSW from hardware and uses it as the base for all
 * subsequent transfers. We mirror that approach. */
static uint32_t g_mem_csw_base = 0;

/* Initialize the MEM-AP CSW once. We read the hardware default but
 * DON'T modify it — the hardware is already correctly configured. */
nrf_ocd_error_t nrf_mem_init_csw(nrf_ap_t *ap) {
    nrf_dap_t *dap = ap->dap;
    uint32_t ap_base = (uint32_t)ap->ap_sel << 24;

    nrf_ocd_error_t err = nrf_ap_read(dap, ap_base | MEM_AP_CSW, &g_mem_csw_base);
    if (err != NRF_OCD_OK)
        return err;

    NRF_DBG("MEM-AP base CSW = 0x%08X", g_mem_csw_base);

    /* Force CSW to a known-good state: SADDRINC + DEVICEEN + 32-bit,
     * preserving HPROT and HNONSEC from the hardware default.
     * Exclude error flags (bits 5-7) that may accumulate. */
    g_mem_csw_base |= CSW_SADDRINC;
    g_mem_csw_base |= CSW_DEVICEEN;
    g_mem_csw_base &= ~0x07U;
    g_mem_csw_base |= CSW_SIZE32;
    g_mem_csw_base &= ~(CSW_SDEVICEEN | CSW_HNONSEC);  /* clear secure debug bits */

    err = nrf_ap_write(dap, ap_base | MEM_AP_CSW, g_mem_csw_base);
    if (err != NRF_OCD_OK) {
        NRF_WARN("MEM-AP CSW init write failed: %s", nrf_ocd_error_str(err));
        return err;
    }

    /* Re-read to verify */
    uint32_t verify;
    err = nrf_ap_read(dap, ap_base | MEM_AP_CSW, &verify);
    NRF_DBG("MEM-AP CSW after init = 0x%08X", verify);

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_mem_read32(nrf_ap_t *ap, uint32_t addr, uint32_t *out) {
    nrf_dap_t *dap = ap->dap;
    uint32_t ap_base = (uint32_t)ap->ap_sel << 24;

    nrf_ocd_error_t err = nrf_ap_write(dap, ap_base | MEM_AP_TAR, addr);
    if (err != NRF_OCD_OK) return err;

    return nrf_ap_read(dap, ap_base | MEM_AP_DRW, out);
}

nrf_ocd_error_t nrf_mem_write32(nrf_ap_t *ap, uint32_t addr, uint32_t data) {
    nrf_dap_t *dap = ap->dap;
    uint32_t ap_base = (uint32_t)ap->ap_sel << 24;

    nrf_ocd_error_t err = nrf_ap_write(dap, ap_base | MEM_AP_TAR, addr);
    if (err != NRF_OCD_OK) return err;

    return nrf_ap_write(dap, ap_base | MEM_AP_DRW, data);
}

nrf_ocd_error_t nrf_mem_read_block32(nrf_ap_t *ap, uint32_t addr, uint32_t *buf, int count) {
    nrf_dap_t *dap = ap->dap;
    uint32_t ap_base = (uint32_t)ap->ap_sel << 24;

    nrf_ocd_error_t err = nrf_ap_write(dap, ap_base | MEM_AP_TAR, addr);
    if (err != NRF_OCD_OK) return err;

    uint8_t request = AP_ACC_BIT | READ_BIT | (uint8_t)MEM_AP_DRW;
    int max_words = ((dap->packet_size > 0 ? dap->packet_size : 64) - 4) / 4;
    if (max_words <= 0)
        return NRF_OCD_ERR_TRANSFER;

    int done = 0;
    while (done < count) {
        int chunk = count - done;
        if (chunk > max_words)
            chunk = max_words;
        err = nrf_dap_transfer_block(dap, 0, (uint16_t)chunk, request, buf + done);
        if (err != NRF_OCD_OK)
            return err;
        done += chunk;
    }

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_mem_write_block32(nrf_ap_t *ap, uint32_t addr, const uint32_t *buf, int count) {
    nrf_dap_t *dap = ap->dap;
    uint32_t ap_base = (uint32_t)ap->ap_sel << 24;

    nrf_ocd_error_t err = nrf_ap_write(dap, ap_base | MEM_AP_TAR, addr);
    if (err != NRF_OCD_OK) return err;

    uint8_t request = AP_ACC_BIT | WRITE_BIT | (uint8_t)MEM_AP_DRW;
    int max_words = ((dap->packet_size > 0 ? dap->packet_size : 64) - 5) / 4;
    if (max_words <= 0)
        return NRF_OCD_ERR_TRANSFER;

    int done = 0;
    while (done < count) {
        int chunk = count - done;
        if (chunk > max_words)
            chunk = max_words;
        err = nrf_dap_transfer_block(dap, 0, (uint16_t)chunk, request, (uint32_t *)(buf + done));
        if (err != NRF_OCD_OK)
            return err;
        done += chunk;
    }

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_mem_write_block8(nrf_ap_t *ap, uint32_t addr, const uint8_t *buf, int count) {
    if (count <= 0)
        return NRF_OCD_OK;

    int i = 0;
    while (i < count) {
        uint32_t word = 0;
        int remaining = count - i;
        int bytes_this_word = remaining < 4 ? remaining : 4;
        memcpy(&word, buf + i, (size_t)bytes_this_word);

        nrf_ocd_error_t err = nrf_mem_write32(ap, addr + (uint32_t)i, word);
        if (err != NRF_OCD_OK)
            return err;

        i += 4;
    }
    return NRF_OCD_OK;
}
