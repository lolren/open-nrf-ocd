/* dap.h - high-level DAP register / memory accessors.
 *
 * Wraps the low-level cmsis_dap_transfer() with the v2-style register
 * read / write helpers and adds memory read / write (which the
 * protocol also calls "ABORT" register + bulk transfer). This is the
 * layer that target.c sits on top of.
 */
#ifndef NRF_OCD_DAP_H
#define NRF_OCD_DAP_H

#include "cmsis_dap.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DP register addresses. */
#define DP_IDCODE        0x0
#define DP_ABORT         0x0
#define DP_CTRL_STAT     0x1
#define DP_WCR           0x1   /* Wait Control. */
#define DP_SELECT        0x2
#define DP_RDBUFF        0x3
#define DP_TARGETID      0x3
#define DP_DLCR          0x3

/* CSW bits. */
#define CSW_DBGSWENABLE  0x80000000U
#define CSW_HPROT        0x03000000U
#define CSW_SPROT        0x01000000U
#define CSW_MASERT       0x00080000U
#define CSW_SPSEL        0x00020000U
#define CSW_SPIDEN       0x00010000U
#define CSW_TRINPROG     0x00000008U
#define CSW_TXERR        0x00000004U
#define CSW_BUSY         0x00000002U
#define CSW_DEVICEEN     0x00000040U
#define CSW_SIZE_WORD    0x00000002U
#define CSW_SIZE_HALF    0x00000001U
#define CSW_SIZE_BYTE    0x00000000U

/* ABORT bits. */
#define ABORT_DAPABORT   0x00000001U
#define ABORT_STKCMPCLR  0x00000002U
#define ABORT_STKERRCLR  0x00000004U
#define ABORT_WDERRCLR   0x00000008U
#define ABORT_ORUNERRCLR 0x00000010U
#define ABORT_ALL_ERR    0x0000001FU

nrf_ocd_status_t dap_dp_read(cmsis_dap_t *dap, uint8_t addr, uint32_t *value);
nrf_ocd_status_t dap_dp_write(cmsis_dap_t *dap, uint8_t addr, uint32_t value);
nrf_ocd_status_t dap_ap_read(cmsis_dap_t *dap, uint8_t ap, uint8_t addr, uint32_t *value);
nrf_ocd_status_t dap_ap_write(cmsis_dap_t *dap, uint8_t ap, uint8_t addr, uint32_t value);

/* Read a block of 32-bit words from an AP register. The CSW must be
 * configured by the caller (auto-increment + size = word). */
nrf_ocd_status_t dap_ap_read_block(cmsis_dap_t *dap, uint8_t ap, uint8_t addr,
                                   uint32_t *out, size_t n);

/* Read / write a contiguous buffer of bytes from / to target memory
 * (assumes the CSW has been set up by the caller). */
nrf_ocd_status_t dap_mem_read(cmsis_dap_t *dap, uint8_t ap, uint32_t addr,
                              void *out, size_t n_bytes);
nrf_ocd_status_t dap_mem_write(cmsis_dap_t *dap, uint8_t ap, uint32_t addr,
                               const void *in, size_t n_bytes);

nrf_ocd_status_t dap_ap_select(cmsis_dap_t *dap, uint8_t ap);
nrf_ocd_status_t dap_ap_csw(cmsis_dap_t *dap, uint8_t ap, uint32_t csw);
nrf_ocd_status_t dap_ap_tar(cmsis_dap_t *dap, uint8_t ap, uint32_t tar);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_DAP_H */
