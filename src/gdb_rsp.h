/* gdb_rsp.h - GDB Remote Serial Protocol client.
 *
 * The GDB Remote Serial Protocol (RSP) is a simple line-oriented protocol
 * that gdbserver speaks. When libusb's bulk backend can't talk to a
 * board (e.g. the Seeed XIAO nRF54's quirky DAPLink firmware), we fall
 * back to spawning `pyocd gdbserver` as a subprocess and driving it
 * via RSP. This gives us a reliable transport that always works
 * because pyOCD handles all the CMSIS-DAP / SWD complexity.
 */
#ifndef NRF_OCD_GDB_RSP_H
#define NRF_OCD_GDB_RSP_H

#include <stddef.h>
#include <stdint.h>

#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gdb_rsp gdb_rsp_t;

typedef struct {
    /* Path to the pyocd executable. If NULL, $PATH is searched. */
    const char *pyocd_path;
    /* Target name (e.g. "nrf54l15"). */
    const char *target;
    /* Probe unique ID. If NULL, the first probe is used. */
    const char *uid;
    /* TCP port for the gdbserver. If 0, an ephemeral port is picked. */
    uint16_t    port;
    /* SWD clock in kHz. 0 means default. */
    int         frequency_khz;
} gdb_rsp_opts_t;

gdb_rsp_t *gdb_rsp_open(const gdb_rsp_opts_t *opts);
void       gdb_rsp_close(gdb_rsp_t *rsp);

/* High-level operations exposed to the rest of nrf_ocd. */
nrf_ocd_status_t gdb_rsp_resume(gdb_rsp_t *rsp);
nrf_ocd_status_t gdb_rsp_halt(gdb_rsp_t *rsp);
nrf_ocd_status_t gdb_rsp_reset(gdb_rsp_t *rsp);
nrf_ocd_status_t gdb_rsp_erase(gdb_rsp_t *rsp);
nrf_ocd_status_t gdb_rsp_flash(gdb_rsp_t *rsp, const char *path);
nrf_ocd_status_t gdb_rsp_mem_read(gdb_rsp_t *rsp, uint32_t addr, void *out, size_t n);
nrf_ocd_status_t gdb_rsp_mem_write(gdb_rsp_t *rsp, uint32_t addr, const void *in, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_GDB_RSP_H */
