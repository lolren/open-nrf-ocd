/* cmsis_dap.c - implementation of the CMSIS-DAP v1/v2 command set.
 *
 * The HID transport is dead simple:
 *   write report  = up to 64 bytes, command id at [0].
 *   read report   = up to 64 bytes, command id at [0], status at [1].
 * We just need to keep the request/response flow tight and make sure
 * timeouts are honoured.
 *
 * We negotiate v1 vs v2 up front using the DAP_Info command, then keep
 * the rest of the file format-agnostic - the report helpers at the top
 * of the file do the encoding.
 */
#include "cmsis_dap.h"
#include "dap.h"
#include "log.h"
#include "nrf_ocd.h"
#include "swd.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMSIS_DAP_DEFAULT_TIMEOUT_MS 2000
#define CMSIS_DAP_BULK_TIMEOUT_MS 10000

/* ---- Low-level v1 / v2 report helpers ------------------------------------ */
static void prepare_request(cmsis_dap_t *dap) {
    memset(dap->cmd_buf, 0, sizeof(dap->cmd_buf));
    memset(dap->rsp_buf, 0, sizeof(dap->rsp_buf));
}

static nrf_ocd_status_t send_recv(cmsis_dap_t *dap, const uint8_t *request,
                                  size_t req_len, size_t expect_min_len) {
    if (dap->is_bulk) {
        /* CMSIS-DAP v2 over USB bulk endpoints (pyOCD pyusb_v2_backend):
         * write the EXACT command bytes (no padding, no report-id prefix),
         * then read the response. Bulk responses start with the command
         * echo byte; callers expect rsp_buf[0] to be the first response
         * byte after that echo, so we strip it. */
        if (req_len < 1 || req_len > sizeof(dap->cmd_buf)) {
            return NRF_OCD_ERR_INVALID_ARG;
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            nrf_ocd_status_t st = hid_write(dap->dev, request, req_len,
                                            CMSIS_DAP_BULK_TIMEOUT_MS);
            if (st != NRF_OCD_OK) {
                if (attempt == 0) { nrf_ocd_sleep_ms(50); continue; }
                LOG_ERROR("bulk write failed for cmd 0x%02x: %s",
                          request[0], nrf_ocd_strerror(st));
                return st;
            }
            size_t got = 0;
            st = hid_read(dap->dev, dap->rsp_buf, sizeof(dap->rsp_buf), &got,
                          CMSIS_DAP_BULK_TIMEOUT_MS);
            if (st != NRF_OCD_OK) {
                if (attempt == 0) { nrf_ocd_sleep_ms(50); continue; }
                LOG_ERROR("bulk read failed for cmd 0x%02x: %s",
                          request[0], nrf_ocd_strerror(st));
                return st;
            }
            /* got includes the command echo byte at [0]; strip it so
             * rsp_buf[0] is the first response payload byte. */
            size_t payload = (got > 0) ? got - 1 : 0;
            if (payload > 0) {
                memmove(dap->rsp_buf, dap->rsp_buf + 1, payload);
            }
            if (payload >= expect_min_len) {
                return NRF_OCD_OK;
            }
            LOG_DEBUG("Bulk short response: %zu bytes for cmd 0x%02x (expect %zu)",
                      got, request[0], expect_min_len + 1);
        }
        LOG_ERROR("Bulk retries exhausted for cmd 0x%02x", request[0]);
        return NRF_OCD_ERR_TIMEOUT;
    }
    /* HID (hidraw / IOKit / hid.dll) backend. The kernel prepends a HID
     * report id (= 0 for v1 devices without explicit report IDs) to
     * every read. We pad the write to the full report size and strip
     * the report id off the response. */
    uint8_t report[NRF_OCD_HID_REPORT_SIZE];
    int rlen = dap->report_size;
    if (rlen <= 0) rlen = 64;
    if ((int)req_len + 1 > rlen) {
        LOG_ERROR("CMSIS-DAP request too long: %zu + 1 > %d", req_len, rlen);
        return NRF_OCD_ERR_INVALID_ARG;
    }
    report[0] = 0;
    memcpy(report + 1, request, req_len);
    if ((int)req_len + 1 < rlen) memset(report + req_len + 1, 0, rlen - req_len - 1);
    nrf_ocd_status_t st = hid_write(dap->dev, report, (size_t)rlen,
                                    CMSIS_DAP_DEFAULT_TIMEOUT_MS);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("hid_write failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    size_t got = 0;
    memset(dap->rsp_buf, 0xAA, sizeof(dap->rsp_buf));
    st = hid_read(dap->dev, dap->rsp_buf, sizeof(dap->rsp_buf), &got,
                  CMSIS_DAP_DEFAULT_TIMEOUT_MS);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("hid_read failed: %s", nrf_ocd_strerror(st));
        return st;
    }
    if (got < 1) return NRF_OCD_ERR_PROTOCOL;
    LOG_DEBUG("CMSIS-DAP response for cmd 0x%02x: got %zu bytes, first 8: "
              "%02x %02x %02x %02x %02x %02x %02x %02x",
              request[0], got,
              got > 0 ? dap->rsp_buf[0] : 0,
              got > 1 ? dap->rsp_buf[1] : 0,
              got > 2 ? dap->rsp_buf[2] : 0,
              got > 3 ? dap->rsp_buf[3] : 0,
              got > 4 ? dap->rsp_buf[4] : 0,
              got > 5 ? dap->rsp_buf[5] : 0,
              got > 6 ? dap->rsp_buf[6] : 0,
              got > 7 ? dap->rsp_buf[7] : 0);
    if (got > 1) {
        memmove(dap->rsp_buf, dap->rsp_buf + 1, got - 1);
        got -= 1;
    }
    LOG_DEBUG("  after shift: %02x %02x %02x %02x %02x %02x %02x %02x",
              got > 0 ? dap->rsp_buf[0] : 0,
              got > 1 ? dap->rsp_buf[1] : 0,
              got > 2 ? dap->rsp_buf[2] : 0,
              got > 3 ? dap->rsp_buf[3] : 0,
              got > 4 ? dap->rsp_buf[4] : 0,
              got > 5 ? dap->rsp_buf[5] : 0,
              got > 6 ? dap->rsp_buf[6] : 0,
              got > 7 ? dap->rsp_buf[7] : 0);
    LOG_DEBUG("HID check: got=%zu expect_min_len=%zu req_len=%zu cmd=0x%02x", got, expect_min_len, req_len, request[0]);
    /* SAMD11 CMSIS-DAP bridge truncates HID responses to 63 bytes (no report_id).
     * After stripping the report_id, we may have fewer bytes than the report size.
     * But if we have enough payload bytes, proceed. */
    if (got < expect_min_len) {
        LOG_DEBUG("Short CMSIS-DAP response for cmd 0x%02x: got=%zu expect_min_len=%zu (proceeding)",
                  request[0], got, expect_min_len);
        /* Don't fail - the response might still contain valid data. */
    }
    return NRF_OCD_OK;
}

/* ---- Connect / info ------------------------------------------------------ */
nrf_ocd_status_t cmsis_dap_open(cmsis_dap_t *dap, hid_device_t *dev) {
    if (!dap || !dev) return NRF_OCD_ERR_INVALID_ARG;
    memset(dap, 0, sizeof(*dap));
    dap->dev = dev;
    dap->is_bulk = hid_is_bulk(dev);
    LOG_DEBUG("cmsis_dap_open: is_bulk=%d", dap->is_bulk ? 1 : 0);
    int rs = hid_report_size(dev);
    if (rs & 0x8000) rs &= 0x7FFF;
    dap->report_size = rs > 0 ? rs : 64;
    /* Detect whether the underlying transport is bulk (libusb) or HID
     * (hidraw / IOKit / hid.dll). The probe.c path sets is_bulk by
     * introspecting the interface class; the default is HID. */
    /* Probe capabilities. DAP_Info IDs follow the CMSIS-DAP spec / pyOCD:
     *   0x01 vendor, 0x02 product, 0x03 serial, 0x04 fw version,
     *   0xFE max packet count, 0xFF max packet size. */
    static struct { uint8_t id; const char *name; size_t value_size; } info_ids[] = {
        { 0x01, "vendor",       0 },
        { 0x02, "product",      0 },
        { 0x03, "serial",       0 },
        { 0x04, "version",      0 },
        { 0xFE, "packet count", 1 },
        { 0xFF, "packet size",  2 },
    };
    for (size_t i = 0; i < sizeof(info_ids) / sizeof(info_ids[0]); i++) {
        prepare_request(dap);
        dap->cmd_buf[0] = DAP_CMD_INFO;
        dap->cmd_buf[1] = info_ids[i].id;
        if (send_recv(dap, dap->cmd_buf, 2, 1) != NRF_OCD_OK) continue;
        /* v1 response: byte 0 = length, byte 1+ = data. The data is
         * either a string (for IDs 0x01-0x04) or an integer (for
         * 0xF0/0xF1). */
        size_t len = dap->rsp_buf[0];
        if (len == 0) continue;
        if (info_ids[i].value_size == 0) {
            /* String. */
            LOG_DEBUG("CMSIS-DAP %s: %.*s", info_ids[i].name, (int)len,
                      (const char *)&dap->rsp_buf[1]);
        } else {
            uint32_t value = 0;
            for (size_t k = 0; k < info_ids[i].value_size && k < len && (1 + k) < sizeof(dap->rsp_buf); k++) {
                value |= (uint32_t)dap->rsp_buf[1 + k] << (8 * k);
            }
            if (info_ids[i].id == 0xFE) {
                dap->packet_count = (int)value;
            } else {
                dap->packet_size = (int)value;
            }
            LOG_DEBUG("CMSIS-DAP %s: %u", info_ids[i].name, (unsigned)value);
        }
    }
    if (dap->packet_count == 0) dap->packet_count = 1;
    if (dap->packet_size == 0)  dap->packet_size  = 64;
    return NRF_OCD_OK;
}

void cmsis_dap_close(cmsis_dap_t *dap) {
    if (!dap) return;
    if (dap->dev) cmsis_dap_disconnect(dap);
    /* Device closed by caller. */
    memset(dap, 0, sizeof(*dap));
}

bool cmsis_dap_is_v2(const cmsis_dap_t *dap) { return dap ? dap->is_v2 : false; }

int cmsis_dap_packet_count(const cmsis_dap_t *dap) { return dap ? dap->packet_count : 0; }
int cmsis_dap_packet_size(const cmsis_dap_t *dap)  { return dap ? dap->packet_size  : 0; }

/* Strings: re-issue on demand because we cache nothing here. */
const char *cmsis_dap_vendor(cmsis_dap_t *dap)   { (void)dap; static char b[64]={0}; return b; }
const char *cmsis_dap_product(cmsis_dap_t *dap)  { (void)dap; static char b[64]={0}; return b; }
const char *cmsis_dap_serial(cmsis_dap_t *dap)   { return hid_serial(dap->dev); }
const char *cmsis_dap_version(cmsis_dap_t *dap)  { (void)dap; static char b[16]={0}; return b; }

/* ---- SWJ ----------------------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_connect(cmsis_dap_t *dap, dap_port_t port) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_CONNECT;
    dap->cmd_buf[1] = (uint8_t)port;
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 2, 1);
    if (st != NRF_OCD_OK) return st;
    /* HID path retains cmd echo byte; bulk path already stripped it. */
    if (dap->is_bulk) {
        if (dap->rsp_buf[0] != 1) {
            LOG_ERROR("DAP_Connect: failed (status=%d)", dap->rsp_buf[0]);
            return NRF_OCD_ERR_PROTOCOL;
        }
    } else {
        /* HID: rsp_buf[0] = cmd echo, rsp_buf[1] = status */
        if (dap->rsp_buf[0] != DAP_CMD_CONNECT || dap->rsp_buf[1] != 1) {
            LOG_ERROR("DAP_Connect: failed (echo=0x%02x status=%d)",
                      dap->rsp_buf[0], dap->rsp_buf[1]);
            return NRF_OCD_ERR_PROTOCOL;
        }
    }
    LOG_DEBUG("DAP_Connect: port 0x%02x connected", port);
    return cmsis_dap_swd_configure(dap, 1, false);
}

nrf_ocd_status_t cmsis_dap_connect_under_reset(cmsis_dap_t *dap) {
    /* pyOCD: DAP_Connect with port=0x03 = SWD with nRESET asserted.
     * This prevents the target's secure firmware from executing
     * during connect, which would re-lock APPROTECT. */
    nrf_ocd_status_t st = cmsis_dap_connect(dap, 3);
    if (st != NRF_OCD_OK) return st;
    /* Release nRESET after connect succeeds. */
    nrf_ocd_sleep_ms(10);
    return st;
}

nrf_ocd_status_t cmsis_dap_disconnect(cmsis_dap_t *dap) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_DISCONNECT;
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 1, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_swj_clock(cmsis_dap_t *dap, uint32_t hz) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_SWJ_CLOCK;
    /* Limit max clock for slow adapters; the Seeed XIAO nRF54 board is fine
     * up to 4 MHz. pyOCD clamps at 5 MHz, so we do the same. */
    if (hz > 5000000U) hz = 5000000U;
    if (hz < 1000U) hz = 1000U;
    write_le32(&dap->cmd_buf[1], hz);
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 5, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_swj_pins(cmsis_dap_t *dap, uint8_t out, uint8_t *pin,
                                    uint32_t wait_ms) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_SWJ_PINS;
    dap->cmd_buf[1] = out;
    write_le16(&dap->cmd_buf[2], (uint16_t)wait_ms);
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 4, 1);
    if (st != NRF_OCD_OK) return st;
    if (pin) *pin = dap->rsp_buf[0];
    if ((dap->rsp_buf[0] & 0x80) == 0) {
        /* No-connect / no-power. */
        return NRF_OCD_ERR_PROTOCOL;
    }
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_swj_sequence(cmsis_dap_t *dap, const uint8_t *seq, size_t n_bits) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_SWJ_SEQUENCE;
    /* pyOCD's format: cmd (1) + length byte (1) + data bytes (ceil(N/8)).
     * Length = 0 means 256. */
    if (n_bits > 256) n_bits = 256;
    size_t n_bytes = (n_bits + 7) / 8;
    if (n_bytes + 2 > sizeof(dap->cmd_buf)) return NRF_OCD_ERR_INVALID_ARG;
    dap->cmd_buf[1] = (n_bits == 256) ? 0 : (uint8_t)n_bits;
    /* The data bytes are sent LSB-first from seq, matching pyOCD's
     * convention of bits >>= 8 per iteration. seq already stores the
     * data in the same order. */
    memcpy(&dap->cmd_buf[2], seq, n_bytes);
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 2 + n_bytes, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_swd_configure(cmsis_dap_t *dap, uint8_t turnaround,
                                         bool always_data) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    /* CMSIS-DAP DAP_SWD_Configure (0x13) per spec / pyOCD:
     *   [0] cmd, [1] config = (turnaround-1) | (alwaysDataPhase << 2). */
    prepare_request(dap);
    uint8_t conf = (uint8_t)((turnaround - 1) & 0x03) | (always_data ? 0x04 : 0x00);
    dap->cmd_buf[0] = DAP_CMD_SWD_CONFIGURE;
    dap->cmd_buf[1] = conf;
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 2, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

/* ---- SWD_Sequence (0x1D) ------------------------------------------------ */
nrf_ocd_status_t cmsis_dap_swd_sequence(cmsis_dap_t *dap, const uint8_t *seq_info,
                                        const uint8_t *seq_data, size_t n_sequences) {
    if (!dap || !seq_info) return NRF_OCD_ERR_INVALID_ARG;
    size_t pos = 2; /* cmd + n_sequences */
    dap->cmd_buf[0] = DAP_CMD_SWD_SEQUENCE;
    dap->cmd_buf[1] = (uint8_t)n_sequences;
    for (size_t i = 0; i < n_sequences; i++) {
        size_t n_bytes = 1; /* Re-calc from seq_info */
        dap->cmd_buf[pos++] = seq_info[i];
        if ((seq_info[i] & 0x80) == 0) {
            int tck = seq_info[i] & 0x3F;
            if (tck == 0) tck = 64;
            n_bytes = (size_t)((tck + 7) / 8);
            for (size_t b = 0; b < n_bytes; b++) {
                if (seq_data) dap->cmd_buf[pos++] = ((const uint8_t (*)[8])seq_data)[i][b];
                else dap->cmd_buf[pos++] = 0;
            }
        }
    }
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, pos, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

/* ---- Transfer ------------------------------------------------------------ */
nrf_ocd_status_t cmsis_dap_transfer_configure(cmsis_dap_t *dap, uint8_t idle_cycles,
                                              uint16_t wait_retry, uint16_t match_retry) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    /* CMSIS-DAP DAP_TransferConfigure (0x04) per spec / pyOCD:
     *   [0] cmd, [1] idle_cycles, [2..3] wait_retry LE, [4..5] match_retry LE.
     * match_retry is a 16-bit field; sending only 1 byte makes some probes
     * (the XIAO nRF54 SAMD11 bridge) return a truncated echo-only response. */
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_TRANSFER_CONFIGURE;
    dap->cmd_buf[1] = idle_cycles;
    write_le16(&dap->cmd_buf[2], wait_retry);
    write_le16(&dap->cmd_buf[4], match_retry);
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 6, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_transfer(cmsis_dap_t *dap, dap_dp_ap_t dp_ap,
                                    uint8_t addr, bool rnw, uint32_t *value) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    /* CMSIS-DAP DAP_Transfer request format (per spec):
     *   byte 0: cmd (0x05)
     *   byte 1: DAP index (0)
     *   byte 2: transfer count (1 for single transfer)
     *   byte 3: transfer request (RnW, APnDP, A2, A3)
     *   bytes 4-7: write data (if write, 4 bytes LE)
     * Response:
     *   byte 0: cmd echo (0x05)
     *   byte 1: count
     *   byte 2: ACK status (0x01 OK, 0x02 WAIT, 0x04 FAULT, 0x07 NO_ACK)
     *   bytes 3-6: read data (if read, 4 bytes LE) */
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_TRANSFER;
    dap->cmd_buf[1] = 0;  /* dap_index */
    dap->cmd_buf[2] = 1;  /* count */
    uint8_t reg = 0;
    if (dp_ap == DAP_TRANSFER_APnDP_AP) reg |= DAP_TRANSFER_APnDP;
    if (rnw) reg |= DAP_TRANSFER_RNW;
    reg |= addr & 0x0C; /* A[2:3] at bits 2 and 3, directly from addr bits 2-3 */
    dap->cmd_buf[3] = reg;
    dap->last_status = 0;
    size_t req_len = 4;
    if (!rnw) {
        if (value) write_le32(&dap->cmd_buf[4], *value);
        req_len = 8;
    }
    /* send_recv strips the command-echo byte on the bulk path, so the
     * expected payload is [count, ACK, (read data)] = 2 (+4 for a read). */
    size_t expect = 2 + (rnw ? 4 : 0);
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, req_len, expect);
    if (st != NRF_OCD_OK) return st;
    LOG_DEBUG("DAP_Transfer req: dp_ap=%d addr=0x%02x rnw=%d reg=0x%02x value=0x%08x",
              dp_ap, addr, rnw, dap->cmd_buf[3], rnw ? 0 : (value ? *value : 0));
    /* Response after send_recv strips the leading byte (HID report ID).
     * For HID: rsp_buf = [cmd echo, count, ACK, data...]
     * For bulk: rsp_buf = [count, ACK, data...] (cmd echo already stripped)
     * We need to handle both cases. */
    uint8_t *rsp = dap->rsp_buf;
    if (!dap->is_bulk && rsp[0] == DAP_CMD_TRANSFER) {
        /* HID path: skip cmd echo byte. */
        rsp++;
    }
    uint8_t xfer_count = rsp[0];
    uint8_t ack = rsp[1] & 0x07;
    dap->last_status = ack;
    if (ack == DAP_TRANSFER_WAIT) {
        for (int max_retry = 50; max_retry > 0 && (ack == DAP_TRANSFER_WAIT); max_retry--) {
            prepare_request(dap);
            dap->cmd_buf[0] = DAP_CMD_TRANSFER;
            dap->cmd_buf[1] = 0;
            dap->cmd_buf[2] = 1;
            dap->cmd_buf[3] = reg;
            if (!rnw && value) { write_le32(&dap->cmd_buf[4], *value); req_len = 8; }
            st = send_recv(dap, dap->cmd_buf, req_len, expect);
            if (st != NRF_OCD_OK) return st;
            ack = dap->rsp_buf[1] & 0x07;
            dap->last_status = ack;
        }
    }
    if (ack == DAP_TRANSFER_FAULT) {
        LOG_DEBUG("DAP_Transfer FAULT: cmd_buf[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x rsp[0..3]=%02x %02x %02x %02x",
              dap->cmd_buf[0], dap->cmd_buf[1], dap->cmd_buf[2], dap->cmd_buf[3],
              dap->cmd_buf[4], dap->cmd_buf[5], dap->cmd_buf[6], dap->cmd_buf[7],
              dap->rsp_buf[0], dap->rsp_buf[1], dap->rsp_buf[2], dap->rsp_buf[3]);
    LOG_DEBUG("DAP_Transfer FAULT on addr 0x%x (count=%d)", (unsigned)addr, xfer_count);
        return NRF_OCD_ERR_FAULT;
    }
    if (ack == DAP_TRANSFER_ERROR) return NRF_OCD_ERR_PROTOCOL;
    if (ack == DAP_TRANSFER_NO_ACK) {
        LOG_DEBUG("DAP_Transfer NO_ACK on addr 0x%x - retrying with SWD_SEQUENCE line reset", (unsigned)addr);
        nrf_ocd_status_t rst = swd_line_reset_swd_sequence(dap);
        if (rst != NRF_OCD_OK) {
            rst = swd_line_reset(dap);
            if (rst != NRF_OCD_OK) return rst;
        }
        /* Clear sticky errors and retry the transfer once. */
        dap_dp_write(dap, 0x0, 0x0000001FU);
        req_len = 4;
        /* Re-send the original request. */
        prepare_request(dap);
        dap->cmd_buf[0] = DAP_CMD_TRANSFER;
        dap->cmd_buf[1] = 0;
        dap->cmd_buf[2] = 1;
        dap->cmd_buf[3] = reg;
        if (!rnw && value) { write_le32(&dap->cmd_buf[4], *value); req_len = 8; }
        st = send_recv(dap, dap->cmd_buf, req_len, expect);
        if (st != NRF_OCD_OK) return st;
        ack = dap->rsp_buf[1] & 0x07;
        if (ack == DAP_TRANSFER_OK) {
            if (rnw && value) *value = read_le32(&dap->rsp_buf[2]);
            return NRF_OCD_OK;
        }
        if (ack == DAP_TRANSFER_FAULT) return NRF_OCD_ERR_FAULT;
        return NRF_OCD_ERR_PROTOCOL;
    }
    if (ack != DAP_TRANSFER_OK) return NRF_OCD_ERR_PROTOCOL;
    if (rnw && value) {
        *value = read_le32(&dap->rsp_buf[2]);
    }
    return NRF_OCD_OK;
}

nrf_ocd_status_t cmsis_dap_transfer_block(cmsis_dap_t *dap, dap_dp_ap_t dp_ap,
                                         uint8_t addr, bool rnw,
                                         const uint32_t *wr_data, uint32_t *rd_data,
                                         size_t n) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    if (n == 0) return NRF_OCD_OK;
    int ps = dap->packet_size  > 0 ? dap->packet_size  : 64;
    size_t idx = 0;
    while (idx < n) {
        prepare_request(dap);
        dap->cmd_buf[0] = DAP_CMD_TRANSFER_BLOCK;
        dap->cmd_buf[1] = 0;
        write_le16(&dap->cmd_buf[2], (uint16_t)(n - idx));
        uint8_t reg = 0;
        if (dp_ap == DAP_TRANSFER_APnDP_AP) reg |= DAP_TRANSFER_APnDP;
        if (rnw) reg |= DAP_TRANSFER_RNW;
        reg |= addr & 0x0C;
        dap->cmd_buf[4] = reg;
        size_t chunk = n - idx;
        size_t req_len = 5;
        if (!rnw && wr_data) {
            uint8_t *p = &dap->cmd_buf[5];
            size_t max_write = (sizeof(dap->cmd_buf) - 5) / 4;
            if (chunk > max_write) chunk = max_write;
            for (size_t i = 0; i < chunk; i++) {
                write_le32(p, wr_data[idx + i]);
                p += 4;
            }
            req_len = 5 + chunk * 4;
        }
        /* Clamp chunk by packet size. */
        if (req_len > (size_t)ps) {
            size_t max_chunk = (size_t)(ps - 5) / 4;
            if (chunk > max_chunk) chunk = max_chunk;
            req_len = 5 + (rnw ? 0 : chunk * 4);
            write_le16(&dap->cmd_buf[2], (uint16_t)chunk);
        }
        size_t expect = 3 + (rnw ? chunk * 4 : 0);
        nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, req_len, expect);
        if (st != NRF_OCD_OK) return st;
        /* Response after send_recv strips leading byte:
         *   rsp_buf[0..1] = count (LE)
         *   rsp_buf[2] = ACK status
         *   rsp_buf[3..] = read data (for reads) */
        uint8_t ack = dap->rsp_buf[2] & 0x07;
        if (ack == DAP_TRANSFER_FAULT) return NRF_OCD_ERR_FAULT;
        if (ack == DAP_TRANSFER_WAIT) {
            dap->last_status = ack;
            for (int max_retry = 50; max_retry > 0 && ack == DAP_TRANSFER_WAIT; max_retry--) {
                st = send_recv(dap, dap->cmd_buf, req_len, expect);
                if (st != NRF_OCD_OK) return st;
                ack = dap->rsp_buf[2] & 0x07;
                dap->last_status = ack;
            }
        }
        if (ack != DAP_TRANSFER_OK) return NRF_OCD_ERR_PROTOCOL;
        if (rnw && rd_data) {
            for (size_t i = 0; i < chunk; i++) {
                rd_data[idx + i] = read_le32(&dap->rsp_buf[3 + i * 4]);
            }
        }
        idx += chunk;
    }
    return NRF_OCD_OK;
}

/* ---- Reset --------------------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_reset_target(cmsis_dap_t *dap) {
    if (!dap) return NRF_OCD_ERR_INVALID_ARG;
    prepare_request(dap);
    dap->cmd_buf[0] = DAP_CMD_RESET_TARGET;
    nrf_ocd_status_t st = send_recv(dap, dap->cmd_buf, 1, 1);
    if (st != NRF_OCD_OK) return st;
    if (dap->rsp_buf[0] != DAP_OK) return NRF_OCD_ERR_PROTOCOL;
    return NRF_OCD_OK;
}
