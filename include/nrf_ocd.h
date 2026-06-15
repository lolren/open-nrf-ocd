/*
 * nrf_ocd.h - Core types and constants for the nrf_ocd flash programmer
 *
 * Implements CMSIS-DAP v1/v2 protocol and nRF54 flash algorithm in pure C.
 * No Python, no external dependencies beyond hidapi.
 */

#ifndef NRF_OCD_H
#define NRF_OCD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ==================== Error codes ==================== */

typedef enum {
    NRF_OCD_OK = 0,
    NRF_OCD_ERR_NO_DEVICE,
    NRF_OCD_ERR_USB_OPEN,
    NRF_OCD_ERR_USB_WRITE,
    NRF_OCD_ERR_USB_READ,
    NRF_OCD_ERR_DAP_CMD,
    NRF_OCD_ERR_SWD_CONNECT,
    NRF_OCD_ERR_TRANSFER,
    NRF_OCD_ERR_TRANSFER_FAULT,
    NRF_OCD_ERR_TRANSFER_WAIT,
    NRF_OCD_ERR_FLASH_INIT,
    NRF_OCD_ERR_FLASH_ERASE,
    NRF_OCD_ERR_FLASH_PROGRAM,
    NRF_OCD_ERR_HEX_PARSE,
    NRF_OCD_ERR_MEMORY,
    NRF_OCD_ERR_TIMEOUT,
    NRF_OCD_ERR_TARGET_MISMATCH,
} nrf_ocd_error_t;

const char *nrf_ocd_error_str(nrf_ocd_error_t err);

/* ==================== USB / Probe ==================== */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    char serial[64];
    char path[512];            /* hidapi path for precise HID opening */
    char vendor[128];
    char product[128];
    void *hid_handle;          /* opaque hid_device * or libusb_device_handle * */
    int report_in_size;
    int report_out_size;
    bool is_v2;                /* CMSIS-DAP v2 (bulk) vs v1 (HID) */
    uint8_t ep_out;            /* libusb bulk OUT endpoint (v2 only) */
    uint8_t ep_in;             /* libusb bulk IN endpoint (v2 only) */
} nrf_probe_t;

/* Enumerate all CMSIS-DAP probes. Returns count, fills array (caller frees). */
nrf_ocd_error_t nrf_probe_enum(nrf_probe_t **out_list, int *out_count);
nrf_ocd_error_t nrf_probe_open(nrf_probe_t *probe);
void nrf_probe_close(nrf_probe_t *probe);
void nrf_probe_free_list(nrf_probe_t **list, int count);
nrf_ocd_error_t nrf_probe_write(nrf_probe_t *probe, const uint8_t *data, int len);
nrf_ocd_error_t nrf_probe_read(nrf_probe_t *probe, uint8_t *buf, int buf_size, int *out_len);

/* ==================== CMSIS-DAP Protocol ==================== */

typedef struct {
    nrf_probe_t *probe;
    uint8_t  protocol_version[3];  /* major, minor, patch */
    int      packet_size;
    int      packet_count;
    int      capabilities;
    bool     connected;
    uint8_t  dap_port;             /* 0=SWD, 1=JTAG */
    uint32_t cached_select;        /* cached DP_SELECT value */
    bool     select_valid;         /* whether cached_select is current */
} nrf_dap_t;

nrf_ocd_error_t nrf_dap_open(nrf_dap_t *dap, nrf_probe_t *probe);
void nrf_dap_close(nrf_dap_t *dap);

/* DAP_Info queries */
nrf_ocd_error_t nrf_dap_info_string(nrf_dap_t *dap, uint8_t id, char *buf, int buf_size);
nrf_ocd_error_t nrf_dap_info_int(nrf_dap_t *dap, uint8_t id, int *out_value);

/* Target connect/disconnect */
nrf_ocd_error_t nrf_dap_connect(nrf_dap_t *dap, uint8_t port);
nrf_ocd_error_t nrf_dap_disconnect(nrf_dap_t *dap);
nrf_ocd_error_t nrf_dap_reset(nrf_dap_t *dap);
nrf_ocd_error_t nrf_dap_set_clock(nrf_dap_t *dap, uint32_t freq_hz);
nrf_ocd_error_t nrf_dap_write_abort(nrf_dap_t *dap, uint32_t data);

/* SWJ pins */
nrf_ocd_error_t nrf_dap_swj_pins(nrf_dap_t *dap, uint8_t output, uint8_t wait, uint8_t pins, uint32_t delay_us, uint8_t *out_value);

/* DAP_Transfer (single read/write) */
nrf_ocd_error_t nrf_dap_transfer(nrf_dap_t *dap, uint8_t dap_index, uint8_t request, uint32_t data, uint32_t *out_data);

/* DAP_TransferBlock (bulk reads) */
nrf_ocd_error_t nrf_dap_transfer_block(nrf_dap_t *dap, uint8_t dap_index, uint16_t count, uint8_t request, uint32_t *out_data);

/* SWD sequence */
nrf_ocd_error_t nrf_dap_swd_sequence(nrf_dap_t *dap, int num_sequences, const uint8_t seq_info[], const uint8_t seq_data[][8], uint8_t out_data[][8]);

/* ==================== CoreSight DP/AP ==================== */

/* DP register addresses */
#define DP_IDCODE       0x00
#define DP_ABORT        0x00
#define DP_CTRL_STAT    0x04
#define DP_RESEND       0x08
#define DP_SELECT       0x08
#define DP_RDBUFF       0x0C
#define DP_TARGETSEL    0x04  /* ADIv6 */
#define DP_TARGETID     0x24

/* DP_CTRL_STAT bits */
#define DP_ABORT_DAPABORT     0x00000001
#define DP_ABORT_STKCMPCLR    0x00000002
#define DP_ABORT_STKERRCLR    0x00000004
#define DP_ABORT_WDERRCLR     0x00000008
#define DP_ABORT_ORUNERRCLR   0x00000010

#define DP_CTRL_ORUNDETECT    0x00000001
#define DP_CTRL_STICKYORUN    0x00000002
#define DP_CTRL_STICKYCMP     0x00000010
#define DP_CTRL_STICKYERR     0x00000020
#define DP_CTRL_WDATAERR      0x00000080
#define DP_CTRL_MASKLANE      0x00000F00
#define DP_CTRL_CDBGPWRUPREQ  0x10000000
#define DP_CTRL_CDBGPWRUPACK  0x20000000
#define DP_CTRL_CSYSPWRUPREQ  0x40000000
#define DP_CTRL_CSYSPWRUPACK  0x80000000

/* AP register offsets */
#define AP_CSW    0x00
#define AP_TAR    0x04
#define AP_DRW    0x0C
#define AP_IDR    0xFC

/* CSW bits */
#define CSW_SIZE8     0x00000000
#define CSW_SIZE16    0x00000001
#define CSW_SIZE32    0x00000002
#define CSW_SADDRINC  0x00000010
#define CSW_DEVICEEN  0x00000040
#define CSW_HPROT     0x10000000  /* HPROT[1] privileged */
#define CSW_SDEVICEEN 0x00800000
#define CSW_HNONSEC   0x40000000

typedef struct {
    nrf_dap_t *dap;
    uint8_t    ap_sel;
    uint32_t   idr;
    uint32_t   rom_addr;
    bool       has_rom_table;
} nrf_ap_t;

nrf_ocd_error_t nrf_dp_read(nrf_dap_t *dap, uint8_t reg, uint32_t *out);
nrf_ocd_error_t nrf_dp_write(nrf_dap_t *dap, uint8_t reg, uint32_t data);
nrf_ocd_error_t nrf_ap_read(nrf_dap_t *dap, uint32_t addr, uint32_t *out);
nrf_ocd_error_t nrf_ap_write(nrf_dap_t *dap, uint32_t addr, uint32_t data);

/* Memory access via MEM-AP */
nrf_ocd_error_t nrf_mem_init_csw(nrf_ap_t *ap);
nrf_ocd_error_t nrf_mem_read32(nrf_ap_t *ap, uint32_t addr, uint32_t *out);
nrf_ocd_error_t nrf_mem_read_block32(nrf_ap_t *ap, uint32_t addr, uint32_t *buf, int count);
nrf_ocd_error_t nrf_mem_write32(nrf_ap_t *ap, uint32_t addr, uint32_t data);
nrf_ocd_error_t nrf_mem_write_block8(nrf_ap_t *ap, uint32_t addr, const uint8_t *buf, int count);
nrf_ocd_error_t nrf_mem_write_block32(nrf_ap_t *ap, uint32_t addr, const uint32_t *buf, int count);

/* SWD connect sequence */
nrf_ocd_error_t nrf_swd_connect(nrf_dap_t *dap);
nrf_ocd_error_t nrf_swd_disconnect(nrf_dap_t *dap);

/* ==================== Flash algorithm for nRF54 ==================== */

/* nRF54L15 flash: 0x00000000 - 0x0017CFFF (1.48MB), UICR at 0x00FFD000 */
/* nRF54LM20A flash: 0x00000000 - 0x001FCFFF (2MB) */

#define NRF54_FLASH_START   0x00000000
#define NRF54L15_FLASH_SIZE 0x0017D000
#define NRF54LM20A_FLASH_SIZE 0x001FD000
#define NRF54_UICR_START    0x00FFD000
#define NRF54_UICR_SIZE     0x1000
#define NRF54_PAGE_SIZE     0x1000  /* 4KB sector */
#define NRF54_MIN_PROG_LEN  4       /* 4 bytes minimum program unit */

/* Flash algo RAM layout */
#define NRF54_ALGO_LOAD_ADDR    0x20000000
#define NRF54_ALGO_INIT         0x20000015
#define NRF54_ALGO_UNINIT       0x20000019
#define NRF54_ALGO_ERASE_ALL    0x2000001D
#define NRF54_ALGO_ERASE_SECTOR 0x20000041
#define NRF54_ALGO_PROGRAM_PAGE 0x20000065
#define NRF54_ALGO_STATIC_BASE  (0x20000000 + 0x04 + 0xA0)
#define NRF54_ALGO_STACK_TOP    0x20000300
#define NRF54_ALGO_PAGE_BUF     0x20001000
#define NRF54_ALGO_PAGE_BUF2    0x20001004

/* CTRL-AP registers (nRF54-specific) */
#define CTRL_AP_RESET           0x000
#define CTRL_AP_ERASEALL        0x004
#define CTRL_AP_ERASEALLSTATUS  0x008
#define CTRL_AP_APPROTECTSTATUS 0x014
#define CTRL_AP_IDR             0x0FC
#define CTRL_AP_IDR_EXPECTED    0x32880000
#define CTRL_AP_ERASEALLSTATUS_BUSY   0x2
#define CTRL_AP_ERASEALLSTATUS_ERROR  0x3
#define CTRL_AP_ERASEALLSTATUS_READYTORESET 0x1

typedef struct {
    const uint32_t *instructions;
    int instruction_count;
    uint32_t load_address;
    uint32_t pc_init;
    uint32_t pc_uninit;
    uint32_t pc_erase_all;
    uint32_t pc_erase_sector;
    uint32_t pc_program_page;
    uint32_t static_base;
    uint32_t stack_top;
    uint32_t page_buffers[2];
    uint32_t page_size;
    uint32_t min_program_length;
} nrf_flash_algo_t;

typedef struct {
    const char *name;
    uint32_t flash_size;
    uint32_t ram_size;
    nrf_flash_algo_t flash_algo;
} nrf_target_desc_t;

extern const nrf_target_desc_t nrf54l15_target;
extern const nrf_target_desc_t nrf54lm20a_target;

typedef struct {
    nrf_ap_t *ap;
    const nrf_target_desc_t *target;
    bool prepared;
    bool inited;
    uint32_t flash_size;
    int  operation;  /* 1=ERASE, 2=PROGRAM */
} nrf_flash_t;

nrf_ocd_error_t nrf_flash_prepare(nrf_flash_t *flash);
nrf_ocd_error_t nrf_flash_init(nrf_flash_t *flash, int operation, uint32_t addr, uint32_t clock);
nrf_ocd_error_t nrf_flash_uninit(nrf_flash_t *flash);
nrf_ocd_error_t nrf_flash_erase_all(nrf_flash_t *flash);
nrf_ocd_error_t nrf_flash_erase_sector(nrf_flash_t *flash, uint32_t addr);
nrf_ocd_error_t nrf_flash_program_page(nrf_flash_t *flash, uint32_t addr, const uint8_t *data, int len);
nrf_ocd_error_t nrf_flash_cleanup(nrf_flash_t *flash);

/* nRF54 CTRL-AP mass erase (used for secure unlock) */
nrf_ocd_error_t nrf54_ctrl_mass_erase(nrf_dap_t *dap);

/* ==================== Intel HEX parser ==================== */

typedef struct {
    uint32_t addr;
    uint8_t  *data;
    int      len;
} nrf_hex_segment_t;

typedef struct {
    nrf_hex_segment_t *segments;
    int count;
    int capacity;
} nrf_hex_file_t;

nrf_ocd_error_t nrf_hex_parse(const char *filename, nrf_hex_file_t *out);
void nrf_hex_free(nrf_hex_file_t *hex);

/* ==================== Main programmer ==================== */

typedef struct {
    nrf_probe_t *probe;
    nrf_dap_t   dap;
    nrf_ap_t    ap;
    nrf_flash_t flash;
    const nrf_target_desc_t *target;
    uint32_t    flash_size;
    bool        is_secure;
    bool        auto_unlock;
    bool        connect_halt;   /* halt core after connect */
    uint32_t    clock_hz;
} nrf_programmer_t;

nrf_ocd_error_t nrf_programmer_init(nrf_programmer_t *prog, nrf_probe_t *probe, const nrf_target_desc_t *target);
void nrf_programmer_close(nrf_programmer_t *prog);
nrf_ocd_error_t nrf_programmer_flash(nrf_programmer_t *prog, const nrf_hex_file_t *hex);
nrf_ocd_error_t nrf_programmer_erase(nrf_programmer_t *prog);
nrf_ocd_error_t nrf_programmer_erase_sector(nrf_programmer_t *prog, uint32_t addr);
nrf_ocd_error_t nrf_programmer_info(nrf_programmer_t *prog);
nrf_ocd_error_t nrf_programmer_read(nrf_programmer_t *prog, uint32_t addr, int len);
nrf_ocd_error_t nrf_programmer_write_word(nrf_programmer_t *prog, uint32_t addr, uint32_t value);
nrf_ocd_error_t nrf_programmer_reset(nrf_programmer_t *prog);

/* ==================== Logging ==================== */

typedef enum {
    NRF_LOG_ERROR = 0,
    NRF_LOG_WARN  = 1,
    NRF_LOG_INFO  = 2,
    NRF_LOG_DEBUG = 3,
} nrf_log_level_t;

void nrf_log_set_level(nrf_log_level_t level);
nrf_log_level_t nrf_log_get_level(void);
void nrf_log(nrf_log_level_t level, const char *fmt, ...);

#define NRF_ERR(...)  nrf_log(NRF_LOG_ERROR, __VA_ARGS__)
#define NRF_WARN(...) nrf_log(NRF_LOG_WARN, __VA_ARGS__)
#define NRF_INFO(...) nrf_log(NRF_LOG_INFO, __VA_ARGS__)
#define NRF_DBG(...)  nrf_log(NRF_LOG_DEBUG, __VA_ARGS__)

#endif /* NRF_OCD_H */
