/*
 * cmsis_dap.c - CMSIS-DAP wire protocol implementation
 *
 * Implements all CMSIS-DAP v1/v2 commands needed for flash programming:
 * DAP_Info, DAP_Connect, DAP_Disconnect, DAP_Transfer, DAP_TransferBlock,
 * DAP_SWJ_Clock, DAP_SWJ_Pins, DAP_SWD_Sequence, DAP_WriteAbort,
 * DAP_ResetTarget, DAP_TransferConfigure
 */

#include "nrf_ocd.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ==================== CMSIS-DAP Command IDs ==================== */

#define CMD_DAP_INFO             0x00
#define CMD_DAP_CONNECT          0x02
#define CMD_DAP_DISCONNECT       0x03
#define CMD_DAP_TRANSFER_CONFIGURE 0x04
#define CMD_DAP_TRANSFER         0x05
#define CMD_DAP_TRANSFER_BLOCK   0x06
#define CMD_DAP_WRITE_ABORT      0x08
#define CMD_DAP_RESET_TARGET     0x0A
#define CMD_DAP_SWJ_PINS         0x10
#define CMD_DAP_SWJ_CLOCK        0x11
#define CMD_DAP_SWD_CONFIGURE    0x13
#define CMD_DAP_SWD_SEQUENCE     0x1D

/* DAP_Info IDs */
#define INFO_CAPABILITIES        0xF0
#define INFO_MAX_PACKET_COUNT    0xFE
#define INFO_MAX_PACKET_SIZE     0xFF
#define INFO_CMSIS_DAP_VER       0x04

/* DAP_Transfer request flags */
#define DP_ACC      0x00
#define AP_ACC      0x01
#define READ        0x02
#define WRITE       0x00

/* DAP_Transfer response ACK values */
#define ACK_OK      0x01
#define ACK_WAIT    0x02
#define ACK_FAULT   0x04
#define ACK_NO_ACK  0x07

/* ==================== Internal helpers ==================== */

static nrf_ocd_error_t dap_send_cmd(nrf_dap_t *dap, const uint8_t *cmd, int cmd_len,
                                     uint8_t *resp, int resp_size, int *resp_len) {
    nrf_ocd_error_t err;

    NRF_DBG("CMD[%d]:", cmd_len);
    for (int i = 0; i < cmd_len && i < 16; i++)
        NRF_DBG("  %02X", cmd[i]);

    err = nrf_probe_write(dap->probe, cmd, cmd_len);
    if (err != NRF_OCD_OK)
        return err;

    uint8_t buf[1024];
    int len = 0;
    err = nrf_probe_read(dap->probe, buf, sizeof(buf), &len);
    if (err != NRF_OCD_OK)
        return err;

    if (len > resp_size)
        len = resp_size;
    memcpy(resp, buf, len);
    *resp_len = len;

    NRF_DBG("RSP[%d]:", len);
    for (int i = 0; i < len && i < 16; i++)
        NRF_DBG("  %02X", buf[i]);

    return NRF_OCD_OK;
}

static nrf_ocd_error_t dap_check_response(uint8_t *resp, int resp_len, uint8_t expected_cmd) {
    if (resp_len < 2)
        return NRF_OCD_ERR_DAP_CMD;
    if (resp[0] != expected_cmd) {
        NRF_ERR("CMSIS-DAP: expected cmd 0x%02X, got 0x%02X", expected_cmd, resp[0]);
        return NRF_OCD_ERR_DAP_CMD;
    }
    return NRF_OCD_OK;
}

/* ==================== DAP_Info ==================== */

nrf_ocd_error_t nrf_dap_info_string(nrf_dap_t *dap, uint8_t id, char *buf, int buf_size) {
    uint8_t cmd[2] = { CMD_DAP_INFO, id };
    uint8_t resp[1024];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 2, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_INFO);
    if (err != NRF_OCD_OK)
        return err;

    int val_len = resp[1];
    if (resp_len < 2 + val_len)
        return NRF_OCD_ERR_DAP_CMD;

    if (val_len == 0) {
        if (buf_size > 0)
            buf[0] = '\0';
        return NRF_OCD_OK;
    }

    int copy_len = val_len;
    if (resp[1 + val_len] == '\0')
        copy_len--;
    if (copy_len >= buf_size)
        copy_len = buf_size - 1;
    if (buf_size > 0) {
        memcpy(buf, resp + 2, copy_len);
        buf[copy_len] = '\0';
    }
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_dap_info_int(nrf_dap_t *dap, uint8_t id, int *out_value) {
    uint8_t cmd[2] = { CMD_DAP_INFO, id };
    uint8_t resp[1024];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 2, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_INFO);
    if (err != NRF_OCD_OK)
        return err;

    int val_len = resp[1];
    if (resp_len < 2 + val_len)
        return NRF_OCD_ERR_DAP_CMD;

    if (val_len == 1) {
        *out_value = resp[2];
    } else if (val_len == 2) {
        *out_value = (resp[3] << 8) | resp[2];
    } else if (val_len == 4) {
        *out_value = (resp[5] << 24) | (resp[4] << 16) | (resp[3] << 8) | resp[2];
    } else {
        return NRF_OCD_ERR_DAP_CMD;
    }
    return NRF_OCD_OK;
}

/* ==================== Open / Close ==================== */

nrf_ocd_error_t nrf_dap_open(nrf_dap_t *dap, nrf_probe_t *probe) {
    nrf_ocd_error_t err;
    int val;

    memset(dap, 0, sizeof(*dap));
    dap->probe = probe;
    dap->connected = false;
    dap->dap_port = 0;
    dap->cached_select = 0;
    dap->select_valid = false;

    err = nrf_probe_open(probe);
    if (err != NRF_OCD_OK)
        return err;

#ifndef _WIN32
    bool dap_info_bad = false;
#endif

    err = nrf_dap_info_int(dap, INFO_MAX_PACKET_SIZE, &val);
    if (err != NRF_OCD_OK) {
        NRF_WARN("Failed to get max packet size, using default 64");
        dap->packet_size = 64;
        dap_info_bad = true;
    } else {
        dap->packet_size = val;
    }

    err = nrf_dap_info_int(dap, INFO_MAX_PACKET_COUNT, &val);
    if (err != NRF_OCD_OK) {
        dap->packet_count = 1;
        dap_info_bad = true;
    } else {
        dap->packet_count = val;
    }

    err = nrf_dap_info_int(dap, INFO_CAPABILITIES, &val);
    if (err != NRF_OCD_OK) {
        dap->capabilities = 0;
        dap_info_bad = true;
    } else {
        dap->capabilities = val;
    }

    char ver_str[32] = {0};
    if (nrf_dap_info_string(dap, INFO_CMSIS_DAP_VER, ver_str, sizeof(ver_str)) == NRF_OCD_OK) {
        sscanf(ver_str, "%hhu.%hhu.%hhu", &dap->protocol_version[0],
                              &dap->protocol_version[1], &dap->protocol_version[2]);
    }

    NRF_INFO("CMSIS-DAP probe: %s %s (serial: %s)", probe->vendor, probe->product, probe->serial);
    NRF_INFO("Packet size: %d, Packet count: %d, Capabilities: 0x%02X",
             dap->packet_size, dap->packet_count, dap->capabilities);

    /* Retry logic for v2 bulk endpoints that get stuck on XIAO SAMD11 bridge.
     * The SAMD11 bridge can leave v2 bulk endpoints in a non-responsive state
     * after mass erase. Closing and reopening the probe with a delay often
     * resets the endpoint state. */
#ifndef _WIN32
    if (probe->is_v2 && dap_info_bad) {
        NRF_WARN("v2 bulk not responding, retrying with fresh connection...");
        nrf_probe_close(probe);
#ifdef _WIN32
        Sleep(1000);
#else
        usleep(1000000);
#endif
        err = nrf_probe_open(probe);
        if (err != NRF_OCD_OK)
            return err;

        dap_info_bad = false;
        dap->packet_size = 64;
        dap->packet_count = 1;
        dap->capabilities = 0;

        err = nrf_dap_info_int(dap, INFO_MAX_PACKET_SIZE, &val);
        if (err == NRF_OCD_OK) dap->packet_size = val;
        else dap_info_bad = true;
        err = nrf_dap_info_int(dap, INFO_MAX_PACKET_COUNT, &val);
        if (err == NRF_OCD_OK) dap->packet_count = val;
        else dap_info_bad = true;
        err = nrf_dap_info_int(dap, INFO_CAPABILITIES, &val);
        if (err == NRF_OCD_OK) dap->capabilities = val;
        else dap_info_bad = true;

        NRF_INFO("v2 retry: packet size=%d, count=%d, caps=0x%02X",
                 dap->packet_size, dap->packet_count, dap->capabilities);

        if (dap_info_bad) {
            NRF_ERR("Probe still not responding. Try unplugging and re-plugging the board.");
            return NRF_OCD_ERR_USB_OPEN;
        }
    }
#endif

    return NRF_OCD_OK;
}

void nrf_dap_close(nrf_dap_t *dap) {
    if (dap->connected)
        nrf_dap_disconnect(dap);
    if (dap->probe)
        nrf_probe_close(dap->probe);
    dap->connected = false;
}

/* ==================== DAP_Connect / Disconnect ==================== */

nrf_ocd_error_t nrf_dap_connect(nrf_dap_t *dap, uint8_t port) {
    uint8_t cmd[2] = { CMD_DAP_CONNECT, port };
    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 2, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_CONNECT);
    if (err != NRF_OCD_OK)
        return err;

    if (resp[1] == 0) {
        NRF_ERR("DAP_Connect failed");
        return NRF_OCD_ERR_SWD_CONNECT;
    }

    dap->connected = true;
    dap->dap_port = resp[1];
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_dap_disconnect(nrf_dap_t *dap) {
    uint8_t cmd[1] = { CMD_DAP_DISCONNECT };
    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 1, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_DISCONNECT);
    if (err != NRF_OCD_OK)
        return err;

    dap->connected = false;
    return NRF_OCD_OK;
}

/* ==================== DAP_ResetTarget ==================== */

nrf_ocd_error_t nrf_dap_reset(nrf_dap_t *dap) {
    uint8_t cmd[1] = { CMD_DAP_RESET_TARGET };
    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 1, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_RESET_TARGET);
    if (err != NRF_OCD_OK)
        return err;

    return NRF_OCD_OK;
}

/* ==================== DAP_SWJ_Clock ==================== */

nrf_ocd_error_t nrf_dap_set_clock(nrf_dap_t *dap, uint32_t freq_hz) {
    uint8_t cmd[5];
    cmd[0] = CMD_DAP_SWJ_CLOCK;
    cmd[1] = (freq_hz >> 0) & 0xFF;
    cmd[2] = (freq_hz >> 8) & 0xFF;
    cmd[3] = (freq_hz >> 16) & 0xFF;
    cmd[4] = (freq_hz >> 24) & 0xFF;

    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 5, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_SWJ_CLOCK);
    if (err != NRF_OCD_OK)
        return err;

    if (resp_len < 2 || resp[1] != 0)
        return NRF_OCD_ERR_DAP_CMD;

    return NRF_OCD_OK;
}

/* ==================== DAP_WriteAbort ==================== */

nrf_ocd_error_t nrf_dap_write_abort(nrf_dap_t *dap, uint32_t data) {
    uint8_t cmd[6];
    cmd[0] = CMD_DAP_WRITE_ABORT;
    cmd[1] = 0;
    cmd[2] = (data >> 0) & 0xFF;
    cmd[3] = (data >> 8) & 0xFF;
    cmd[4] = (data >> 16) & 0xFF;
    cmd[5] = (data >> 24) & 0xFF;

    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 6, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_WRITE_ABORT);
    if (err != NRF_OCD_OK)
        return err;

    if (resp_len < 2 || resp[1] != 0)
        return NRF_OCD_ERR_DAP_CMD;

    return NRF_OCD_OK;
}

/* ==================== DAP_SWJ_Pins ==================== */

nrf_ocd_error_t nrf_dap_swj_pins(nrf_dap_t *dap, uint8_t output, uint8_t wait, uint8_t pins,
                                  uint32_t delay_us, uint8_t *out_value) {
    (void)wait;
    uint8_t cmd[8];
    cmd[0] = CMD_DAP_SWJ_PINS;
    cmd[1] = output;
    cmd[2] = pins;
    cmd[3] = (delay_us >> 0) & 0xFF;
    cmd[4] = (delay_us >> 8) & 0xFF;
    cmd[5] = (delay_us >> 16) & 0xFF;
    cmd[6] = (delay_us >> 24) & 0xFF;

    uint8_t resp[64];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, 7, resp, sizeof(resp), &resp_len);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_SWJ_PINS);
    if (err != NRF_OCD_OK)
        return err;

    if (out_value)
        *out_value = resp[1];
    return NRF_OCD_OK;
}

/* ==================== DAP_SWD_Sequence ==================== */

nrf_ocd_error_t nrf_dap_swd_sequence(nrf_dap_t *dap, int num_sequences,
                                      const uint8_t seq_info[], const uint8_t seq_data[][8],
                                      uint8_t out_data[][8]) {
    uint8_t *cmd = malloc(512);
    if (!cmd)
        return NRF_OCD_ERR_MEMORY;

    int pos = 0;
    cmd[pos++] = CMD_DAP_SWD_SEQUENCE;
    cmd[pos++] = (uint8_t)num_sequences;

    for (int i = 0; i < num_sequences; i++) {
        cmd[pos++] = seq_info[i];
        if ((seq_info[i] & 0x80) == 0) {
            int tck = seq_info[i] & 0x3F;
            if (tck == 0) tck = 64;
            int bytes_needed = (tck + 7) / 8;
            for (int b = 0; b < bytes_needed && b < 8; b++) {
                cmd[pos++] = seq_data[i][b];
            }
        }
    }

    uint8_t resp[512];
    int resp_len = 0;

    nrf_ocd_error_t err = dap_send_cmd(dap, cmd, pos, resp, sizeof(resp), &resp_len);
    free(cmd);
    if (err != NRF_OCD_OK)
        return err;

    err = dap_check_response(resp, resp_len, CMD_DAP_SWD_SEQUENCE);
    if (err != NRF_OCD_OK)
        return err;

    if (resp[1] != 0)
        return NRF_OCD_ERR_DAP_CMD;

    int offset = 2;
    for (int i = 0; i < num_sequences; i++) {
        if (seq_info[i] & 0x80) {
            int tck = seq_info[i] & 0x3F;
            if (tck == 0) tck = 64;
            int bytes_needed = (tck + 7) / 8;
            for (int b = 0; b < bytes_needed && b < 8; b++) {
                if (offset + b < resp_len)
                    out_data[i][b] = resp[offset + b];
                else
                    out_data[i][b] = 0;
            }
            offset += bytes_needed;
        }
    }

    return NRF_OCD_OK;
}

/* ==================== DAP_Transfer ==================== */

/* Max SW retries for WAIT responses before escalating to DAP_WriteAbort. */
#define MAX_WAIT_RETRIES   64
#define MAX_FAULT_RETRIES  3

/* Internal: clear DP sticky error flags. */
static nrf_ocd_error_t dap_clear_sticky(nrf_dap_t *dap) {
    dap->select_valid = false;  /* invalidate cache after abort */
    return nrf_dp_write(dap, DP_ABORT,
        DP_ABORT_ORUNERRCLR | DP_ABORT_WDERRCLR |
        DP_ABORT_STKERRCLR | DP_ABORT_STKCMPCLR);
}

/* Internal: abort a stuck transfer via DAP_WriteAbort. */
static nrf_ocd_error_t dap_abort_transfer(nrf_dap_t *dap) {
    dap->select_valid = false;
    return nrf_dap_write_abort(dap, DP_ABORT_DAPABORT);
}

/* Internal: SWD line reset sequence (>50 clocks high, 8 idle, read DP IDR). */
static nrf_ocd_error_t dap_swd_line_reset(nrf_dap_t *dap) {
    nrf_ocd_error_t err;

    dap->select_valid = false;

    /* SWJ sequence: 51+ clocks high, then 8 idle */
    {
        uint8_t pins;
        err = nrf_dap_swj_pins(dap, 0xFF, 0, 0xFF, 100, &pins);
        if (err != NRF_OCD_OK) return err;
    }

    /* Short delay for line reset to settle */
    {
        uint8_t pins;
        err = nrf_dap_swj_pins(dap, 0x00, 0, 0xFF, 100, &pins);
        if (err != NRF_OCD_OK) return err;
    }

    /* Read DP IDCODE to exit reset state */
    uint32_t idr;
    err = nrf_dp_read(dap, DP_IDCODE, &idr);
    if (err != NRF_OCD_OK) {
        NRF_DBG("SWD line reset: DP IDR read failed (%s)", nrf_ocd_error_str(err));
        return err;
    }

    NRF_DBG("SWD line reset complete, DP IDCODE = 0x%08X", idr);
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_dap_transfer(nrf_dap_t *dap, uint8_t dap_index, uint8_t request, uint32_t data, uint32_t *out_data) {
    int fault_retries = 0;
    bool did_swd_reset = false;

    for (;;) {
        uint8_t cmd[32];
        int pos = 0;

        cmd[pos++] = CMD_DAP_TRANSFER;
        cmd[pos++] = dap_index;
        cmd[pos++] = 1;
        cmd[pos++] = request;

        if (!(request & READ)) {
            cmd[pos++] = (data >> 0) & 0xFF;
            cmd[pos++] = (data >> 8) & 0xFF;
            cmd[pos++] = (data >> 16) & 0xFF;
            cmd[pos++] = (data >> 24) & 0xFF;
        }

        uint8_t resp[256];
        int resp_len = 0;

        nrf_ocd_error_t err = dap_send_cmd(dap, cmd, pos, resp, sizeof(resp), &resp_len);
        if (err != NRF_OCD_OK)
            return err;

        if (resp_len < 3) {
            NRF_DBG("DAP_Transfer: response too short (%d bytes)", resp_len);
            return NRF_OCD_ERR_TRANSFER;
        }

        err = dap_check_response(resp, resp_len, CMD_DAP_TRANSFER);
        if (err != NRF_OCD_OK)
            return err;

        if (resp[1] != 1) {
            /* Count mismatch — check the ACK to determine actual error type. */
            uint8_t short_ack = resp[2] & 0x07;
            if (short_ack == ACK_WAIT) {
                NRF_DBG("DAP_Transfer: ACK_WAIT (count=%d)", resp[1]);
                static int wait_count = 0;
                wait_count++;
                if (wait_count > MAX_WAIT_RETRIES) {
                    wait_count = 0;
                    dap_abort_transfer(dap);
                }
                continue;
            }
            if (short_ack == ACK_FAULT) {
                NRF_DBG("DAP_Transfer: ACK_FAULT (count=%d)", resp[1]);
                fault_retries++;
                if (fault_retries > MAX_FAULT_RETRIES) {
                    NRF_WARN("DAP_Transfer: FAULT retries exhausted (low-count, ack=0x%02X)", short_ack);
                    return NRF_OCD_ERR_TRANSFER_FAULT;
                }
                dap_clear_sticky(dap);
                continue;
            }
            NRF_DBG("DAP_Transfer: count mismatch expected=1 got=%d ack=0x%02X",
                    resp[1], short_ack);
            return NRF_OCD_ERR_TRANSFER;
        }

        /* Check for protocol error flag in response byte */
        if (resp[2] & 0x08) {
            NRF_DBG("DAP_Transfer: protocol error flag set");
            if (!did_swd_reset) {
                NRF_WARN("DAP_Transfer: protocol error, attempting SWD line reset");
                did_swd_reset = true;
                dap->select_valid = false;

                nrf_ocd_error_t rerr = dap_swd_line_reset(dap);
                if (rerr != NRF_OCD_OK) {
                    NRF_ERR("SWD line reset failed: %s", nrf_ocd_error_str(rerr));
                    return NRF_OCD_ERR_TRANSFER;
                }
                continue;
            }
            return NRF_OCD_ERR_TRANSFER;
        }

        uint8_t ack = resp[2] & 0x07;

        if (ack == ACK_OK) {
            if (request & READ && out_data) {
                if (resp_len < 7)
                    return NRF_OCD_ERR_TRANSFER;
                *out_data = (uint32_t)resp[3] |
                            ((uint32_t)resp[4] << 8) |
                            ((uint32_t)resp[5] << 16) |
                            ((uint32_t)resp[6] << 24);
            }
            return NRF_OCD_OK;
        }

        if (ack == ACK_WAIT) {
            NRF_DBG("DAP_Transfer: ACK_WAIT, retrying...");
            /* The probe handles WAIT retries internally (TransferConfigure),
             * but we still retry at the SW level for robustness. After too many
             * retries, abort the transfer and try once more. */
            static int wait_count = 0;
            wait_count++;
            if (wait_count > MAX_WAIT_RETRIES) {
                wait_count = 0;
                NRF_DBG("DAP_Transfer: WAIT retries exhausted, sending DAP_WriteAbort");
                dap_abort_transfer(dap);
            }
            continue;
        }

        if (ack == ACK_FAULT) {
            NRF_DBG("DAP_Transfer: ACK_FAULT, clearing sticky and retrying");
            fault_retries++;
            if (fault_retries > MAX_FAULT_RETRIES) {
                NRF_WARN("DAP_Transfer: FAULT retries exhausted (req=0x%02X)", request);
                return NRF_OCD_ERR_TRANSFER_FAULT;
            }
            dap_clear_sticky(dap);
            continue;
        }

        NRF_DBG("DAP_Transfer: bad ACK=0x%02X resp_len=%d", ack, resp_len);
        return NRF_OCD_ERR_TRANSFER;
    }
}

/* ==================== DAP_TransferBlock ==================== */

nrf_ocd_error_t nrf_dap_transfer_block(nrf_dap_t *dap, uint8_t dap_index, uint16_t count, uint8_t request, uint32_t *out_data) {
    int fault_retries = 0;
    bool did_swd_reset = false;

    if (count == 0)
        return NRF_OCD_OK;
    if (!(request & READ) && !out_data)
        return NRF_OCD_ERR_TRANSFER;

    for (;;) {
        uint8_t cmd[1024];
        int pos = 0;
        int packet_size = dap->packet_size > 0 ? dap->packet_size : 64;
        if (packet_size > (int)sizeof(cmd))
            packet_size = sizeof(cmd);

        int max_count = (request & READ) ? ((packet_size - 4) / 4) : ((packet_size - 5) / 4);
        if (max_count <= 0 || count > (uint16_t)max_count)
            return NRF_OCD_ERR_TRANSFER;

        cmd[pos++] = CMD_DAP_TRANSFER_BLOCK;
        cmd[pos++] = dap_index;
        cmd[pos++] = count & 0xFF;
        cmd[pos++] = (count >> 8) & 0xFF;
        cmd[pos++] = request;

        if (!(request & READ)) {
            for (uint16_t i = 0; i < count; i++) {
                cmd[pos++] = (out_data[i] >> 0) & 0xFF;
                cmd[pos++] = (out_data[i] >> 8) & 0xFF;
                cmd[pos++] = (out_data[i] >> 16) & 0xFF;
                cmd[pos++] = (out_data[i] >> 24) & 0xFF;
            }
        }

        uint8_t resp[1024];
        int resp_len = 0;

        nrf_ocd_error_t err = dap_send_cmd(dap, cmd, pos, resp, sizeof(resp), &resp_len);
        if (err != NRF_OCD_OK)
            return err;

        err = dap_check_response(resp, resp_len, CMD_DAP_TRANSFER_BLOCK);
        if (err != NRF_OCD_OK)
            return err;

        if (resp_len < 4)
            return NRF_OCD_ERR_TRANSFER;

        uint16_t resp_count = resp[1] | ((uint16_t)resp[2] << 8);
        if (resp_count != count) {
            /* Count mismatch — check the ACK for actual error type. */
            uint8_t short_ack = resp[3] & 0x07;
            if (short_ack == ACK_WAIT) {
                NRF_DBG("DAP_TransferBlock: ACK_WAIT (count=%d)", resp_count);
                continue;
            }
            if (short_ack == ACK_FAULT) {
                NRF_DBG("DAP_TransferBlock: ACK_FAULT (count=%d)", resp_count);
                fault_retries++;
                if (fault_retries > MAX_FAULT_RETRIES) {
                    NRF_WARN("DAP_TransferBlock: FAULT retries exhausted");
                    return NRF_OCD_ERR_TRANSFER_FAULT;
                }
                dap_clear_sticky(dap);
                continue;
            }
            NRF_DBG("DAP_TransferBlock: count mismatch expected=%d got=%d ack=0x%02X",
                    count, resp_count, short_ack);
            return NRF_OCD_ERR_TRANSFER;
        }

        /* Protocol error flag */
        if (resp[3] & 0x08) {
            NRF_DBG("DAP_TransferBlock: protocol error flag set");
            if (!did_swd_reset) {
                NRF_WARN("DAP_TransferBlock: protocol error, attempting SWD line reset");
                did_swd_reset = true;
                dap->select_valid = false;
                nrf_ocd_error_t rerr = dap_swd_line_reset(dap);
                if (rerr != NRF_OCD_OK) {
                    NRF_ERR("SWD line reset failed: %s", nrf_ocd_error_str(rerr));
                    return NRF_OCD_ERR_TRANSFER;
                }
                continue;
            }
            return NRF_OCD_ERR_TRANSFER;
        }

        uint8_t ack = resp[3] & 0x07;

        if (ack == ACK_OK) {
            if (request & READ && out_data) {
                int data_offset = 4;
                for (uint16_t i = 0; i < count; i++) {
                    if (data_offset + 3 >= resp_len)
                        return NRF_OCD_ERR_TRANSFER;
                    out_data[i] = (uint32_t)resp[data_offset] |
                                  ((uint32_t)resp[data_offset + 1] << 8) |
                                  ((uint32_t)resp[data_offset + 2] << 16) |
                                  ((uint32_t)resp[data_offset + 3] << 24);
                    data_offset += 4;
                }
            }
            return NRF_OCD_OK;
        }

        if (ack == ACK_WAIT) {
            NRF_DBG("DAP_TransferBlock: ACK_WAIT");
            continue;
        }

        if (ack == ACK_FAULT) {
            NRF_DBG("DAP_TransferBlock: ACK_FAULT, clearing sticky");
            fault_retries++;
            if (fault_retries > MAX_FAULT_RETRIES) {
                NRF_WARN("DAP_TransferBlock: FAULT retries exhausted");
                return NRF_OCD_ERR_TRANSFER_FAULT;
            }
            dap_clear_sticky(dap);
            continue;
        }

        NRF_DBG("DAP_TransferBlock: bad ACK=0x%02X", ack);
        return NRF_OCD_ERR_TRANSFER;
    }
}

/* ==================== DP Read/Write ==================== */

nrf_ocd_error_t nrf_dp_read(nrf_dap_t *dap, uint8_t reg, uint32_t *out) {
    uint8_t request = DP_ACC | READ | (reg & 0x0C);
    return nrf_dap_transfer(dap, 0, request, 0, out);
}

nrf_ocd_error_t nrf_dp_write(nrf_dap_t *dap, uint8_t reg, uint32_t data) {
    uint8_t request = DP_ACC | WRITE | (reg & 0x0C);
    return nrf_dap_transfer(dap, 0, request, data, NULL);
}

/* ==================== AP Read/Write ==================== */

nrf_ocd_error_t nrf_ap_read(nrf_dap_t *dap, uint32_t addr, uint32_t *out) {
    uint32_t select = addr & 0xFF0000F0;

    /* Cache DP_SELECT: skip write if already current. */
    if (!dap->select_valid || dap->cached_select != select) {
        nrf_ocd_error_t err = nrf_dp_write(dap, DP_SELECT, select);
        if (err != NRF_OCD_OK)
            return err;
        dap->cached_select = select;
        dap->select_valid = true;
    }

    uint8_t request = AP_ACC | READ | (addr & 0x0C);
    return nrf_dap_transfer(dap, 0, request, 0, out);
}

nrf_ocd_error_t nrf_ap_write(nrf_dap_t *dap, uint32_t addr, uint32_t data) {
    uint32_t select = addr & 0xFF0000F0;

    if (!dap->select_valid || dap->cached_select != select) {
        nrf_ocd_error_t err = nrf_dp_write(dap, DP_SELECT, select);
        if (err != NRF_OCD_OK)
            return err;
        dap->cached_select = select;
        dap->select_valid = true;
    }

    uint8_t request = AP_ACC | WRITE | (addr & 0x0C);
    return nrf_dap_transfer(dap, 0, request, data, NULL);
}

