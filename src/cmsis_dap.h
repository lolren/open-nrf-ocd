/* cmsis_dap.h - CMSIS-DAP command set over HID.
 *
 * Implements both v1 and v2 transports, picks the right one at connect()
 * time, and provides the DAP operations used by swd.c, dap.c and the
 * flash programming engine. The class deliberately mirrors pyOCD's
 * CMSISDAP class so the rest of the codebase can stay close to pyOCD's
 * structure.
 *
 * Wire format (v1, the Seeed board default):
 *   Report[0] = command id
 *   Report[1..] = command data
 *   Response[0] = command id
 *   Response[1] = status (DAP_OK, DAP_ERROR, ...)
 *   Response[2..] = response data
 *   Reports are 64 bytes long; unused trailing bytes are zero.
 *
 * Wire format (v2):
 *   Report[0] = command id
 *   Report[1..2] = data length (LE16)
 *   Report[3..2+len] = data
 *   Response[0] = command id
 *   Response[1..2] = data length (LE16)
 *   Response[3]   = status (DAPv2 rcode)
 *   Response[4..] = response data
 */
#ifndef NRF_OCD_CMSIS_DAP_H
#define NRF_OCD_CMSIS_DAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CMSIS-DAP command ids. */
#define DAP_CMD_INFO                    0x00
#define DAP_CMD_HOST_STATUS             0x01
#define DAP_CMD_CONNECT                 0x02
#define DAP_CMD_DISCONNECT              0x03
#define DAP_CMD_TRANSFER_CONFIGURE      0x04
#define DAP_CMD_TRANSFER                0x05
#define DAP_CMD_TRANSFER_BLOCK          0x06
#define DAP_CMD_TRANSFER_ABORT          0x07
#define DAP_CMD_WRITE_ABORT             0x08
#define DAP_CMD_DELAY                   0x09
#define DAP_CMD_RESET_TARGET            0x0A
#define DAP_CMD_SWJ_PINS                0x10
#define DAP_CMD_SWJ_CLOCK               0x11
#define DAP_CMD_SWJ_SEQUENCE            0x12
#define DAP_CMD_SWD_CONFIGURE           0x13
#define DAP_CMD_JTAG_SEQUENCE           0x14
#define DAP_CMD_JTAG_CONFIGURE          0x15
#define DAP_CMD_JTAG_IDCODE             0x16
#define DAP_CMD_SWD_SEQUENCE            0x1D
#define DAP_CMD_VENDOR0                 0x80

/* Response status codes (v1). */
#define DAP_OK                          0x00
#define DAP_ERROR                       0xFF

/* DAP transfer response codes (v1 status[1] of the transfer response). */
#define DAP_TRANSFER_OK                 0x01
#define DAP_TRANSFER_WAIT               0x02
#define DAP_TRANSFER_FAULT              0x04
#define DAP_TRANSFER_ERROR              0x08
#define DAP_TRANSFER_MISMATCH           0x10
#define DAP_TRANSFER_NO_ACK             0x07  /* Bits 0+1+2 = 7 = ACK_NO_ACK */

/* DAP_TRANSFER RnW/APnDP/A[2:3] masks (CMSIS-DAP v1 spec: bit0=APnDP, bit1=RnW). */
#define DAP_TRANSFER_APnDP              0x01
#define DAP_TRANSFER_RNW                0x02
#define DAP_TRANSFER_A2                 0x04
#define DAP_TRANSFER_A3                 0x08
#define DAP_TRANSFER_POSTED             0x10
#define DAP_TRANSFER_TIMESTAMP          0x20

typedef enum {
    DAP_PORT_SWD = 1,
    DAP_PORT_JTAG = 2,
} dap_port_t;

typedef enum {
    DAP_TRANSFER_APnDP_DP = 0,
    DAP_TRANSFER_APnDP_AP = 1,
} dap_dp_ap_t;

typedef struct {
    hid_device_t *dev;
    bool          is_v2;
    bool          is_bulk;       /* libusb bulk backend - no HID report id. */
    int           report_size;   /* usually 64. */
    int           packet_count;  /* max transfers per HID report. */
    int           packet_size;   /* max transfer bytes per packet. */
    uint8_t       cmd_buf[1024];
    uint8_t       rsp_buf[1024];
    /* Most recent SWD transfer's post-ack turnaround state - used for
     * line-direction bookkeeping between successive SWD ops. */
    uint32_t      last_status;   /* sticky DAP_TRANSFER_*  */
} cmsis_dap_t;

/* ---- Connection / info ---------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_open(cmsis_dap_t *dap, hid_device_t *dev);
void             cmsis_dap_close(cmsis_dap_t *dap);
const char      *cmsis_dap_vendor(cmsis_dap_t *dap);
const char      *cmsis_dap_product(cmsis_dap_t *dap);
const char      *cmsis_dap_serial(cmsis_dap_t *dap);
const char      *cmsis_dap_version(cmsis_dap_t *dap);
int              cmsis_dap_packet_count(const cmsis_dap_t *dap);
int              cmsis_dap_packet_size(const cmsis_dap_t *dap);
bool             cmsis_dap_is_v2(const cmsis_dap_t *dap);

/* ---- SWJ / clock ---------------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_connect(cmsis_dap_t *dap, dap_port_t port);
nrf_ocd_status_t cmsis_dap_disconnect(cmsis_dap_t *dap);
nrf_ocd_status_t cmsis_dap_swj_clock(cmsis_dap_t *dap, uint32_t hz);
nrf_ocd_status_t cmsis_dap_swj_pins(cmsis_dap_t *dap, uint8_t out, uint8_t *pin, uint32_t wait_ms);
nrf_ocd_status_t cmsis_dap_swj_sequence(cmsis_dap_t *dap, const uint8_t *seq, size_t n_bits);
/* SWD_Sequence sends bit sequences specifically for SWD (more reliable than
 * SWJ_Sequence on the XIAO nRF54 board). The format encodes multiple
 * sequences in one command. */
nrf_ocd_status_t cmsis_dap_swd_sequence(cmsis_dap_t *dap, const uint8_t *seq_info,
                                        const uint8_t *seq_data, size_t n_sequences);
nrf_ocd_status_t cmsis_dap_swd_configure(cmsis_dap_t *dap, uint8_t turnaround,
                                         bool always_data);

/* ---- Transfer ------------------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_transfer_configure(cmsis_dap_t *dap, uint8_t idle_cycles,
                                              uint16_t wait_retry, uint16_t match_retry);

nrf_ocd_status_t cmsis_dap_transfer(cmsis_dap_t *dap, dap_dp_ap_t dp_ap,
                                    uint8_t addr, bool rnw, uint32_t *value);

nrf_ocd_status_t cmsis_dap_transfer_block(cmsis_dap_t *dap, dap_dp_ap_t dp_ap,
                                         uint8_t addr, bool rnw,
                                         const uint32_t *wr_data, uint32_t *rd_data,
                                         size_t n);

/* ---- Reset ---------------------------------------------------------------- */
nrf_ocd_status_t cmsis_dap_reset_target(cmsis_dap_t *dap);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_CMSIS_DAP_H */
