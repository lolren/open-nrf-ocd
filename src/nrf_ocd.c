/*
 * nrf_ocd.c - CLI entry point for nrf_ocd
 *
 * nrf_ocd - Native C CMSIS-DAP flash programmer for nRF54L15 / nRF54LM20A
 * Portable, zero-dependency pyOCD replacement.
 *
 * Usage:
 *   nrf_ocd -l                            List probes
 *   nrf_ocd -u <serial> -i                Device info
 *   nrf_ocd -u <serial> -e                Mass erase
 *   nrf_ocd -u <serial> -e -s 0xADDR      Sector erase
 *   nrf_ocd -u <serial> -f <file.hex>     Flash
 *   nrf_ocd -u <serial> -f <hex> -e       Erase + flash
 *   nrf_ocd -u <serial> -r ADDR LEN       Read memory
 *   nrf_ocd -u <serial> -w ADDR VALUE     Write 32-bit word
 *   nrf_ocd -u <serial> -R                Reset target
 *   nrf_ocd -u <serial> --auto-unlock -f <hex>  Auto-unlock secure device
 *
 * Targets: nrf54l15 (default), nrf54lm20a
 */

#include "nrf_ocd.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

typedef enum {
    ACTION_NONE = 0,
    ACTION_LIST,
    ACTION_FLASH,
    ACTION_ERASE,
    ACTION_INFO,
    ACTION_READ,
    ACTION_WRITE,
    ACTION_RESET,
} action_t;

static const nrf_target_desc_t *target_from_name(const char *name) {
    if (!name || strcmp(name, "nrf54l15") == 0)
        return &nrf54l15_target;
    if (strcmp(name, "nrf54lm20a") == 0)
        return &nrf54lm20a_target;
    return NULL;
}

static bool parse_u32_arg(const char *s, uint32_t *out) {
    char *end = NULL;
    if (!s || !*s) return false;
    unsigned long value = strtoul(s, &end, 0);
    if (!end || *end != '\0' || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "nrf_ocd - Native CMSIS-DAP flash programmer for nRF54 (pyOCD replacement)\n"
        "\n"
        "Commands:\n"
        "  %s -l                              List connected CMSIS-DAP probes\n"
        "  %s -u <serial> -i                  Show target device info\n"
        "  %s -u <serial> -e                  Mass erase all flash\n"
        "  %s -u <serial> -e -s ADDR          Sector erase at address\n"
        "  %s -u <serial> -f <file.hex>       Program .hex file\n"
        "  %s -u <serial> -f <hex> -e         Erase then program\n"
        "  %s -u <serial> -r ADDR LEN         Read LEN bytes from ADDR (hex dump)\n"
        "  %s -u <serial> -w ADDR VALUE       Write 32-bit word to ADDR\n"
        "  %s -u <serial> -R                  Reset target\n"
        "\n"
        "Options:\n"
        "  -t <target>   Target: nrf54l15 (default), nrf54lm20a\n"
        "  -c <freq>     SWD clock in Hz (default: 4000000, max: 8000000)\n"
        "  --auto-unlock Auto mass-erase locked devices\n"
        "  --connect MODE Connect mode: halt (default), attach\n"
        "  --no-reset    Don't reset after programming\n"
        "  -v            Verbose debug output\n"
        "  -q            Quiet mode\n"
        "  -h            Show this help\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static const char *nrf_serial_from_port(const char *port_path);

int main(int argc, char *argv[]) {
    action_t action = ACTION_NONE;
    char *serial = NULL;
    char *hex_file = NULL;
    char *target = NULL;
    char *connect_mode = NULL;
    uint32_t clock_hz = 4000000;
    uint32_t read_addr = 0, write_addr = 0, write_value = 0;
    uint32_t sector_addr = 0;
    int read_len = 64;
    bool auto_unlock = false;
    bool do_erase = false;
    bool no_reset __attribute__((unused)) = false;
    bool do_reset = false;
    bool has_sector = false;
    const char *port_path = NULL;

    static struct option long_options[] = {
        {"auto-unlock", no_argument,       NULL, 'U'},
        {"no-reset",    no_argument,       NULL, 'n'},
        {"connect",     required_argument, NULL, 'm'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL,          0,                 NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "lu:f:et:c:vqihr:w:Rs:p:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'l': action = ACTION_LIST; break;
            case 'p': port_path = optarg; break;
            case 'u': serial = optarg; break;
            case 'f': hex_file = optarg; action = ACTION_FLASH; break;
            case 'e': do_erase = true; if (action == ACTION_NONE) action = ACTION_ERASE; break;
            case 't': target = optarg; break;
            case 'c':
                if (!parse_u32_arg(optarg, &clock_hz) || clock_hz == 0) {
                    fprintf(stderr, "Error: invalid SWD clock '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'v': nrf_log_set_level(NRF_LOG_DEBUG); break;
            case 'q': nrf_log_set_level(NRF_LOG_ERROR); break;
            case 'U': auto_unlock = true; break;
            case 'n': no_reset = true; break;
            case 'm': connect_mode = optarg; break;
            case 'i': action = ACTION_INFO; break;
            case 'r':
                action = ACTION_READ;
                if (!parse_u32_arg(optarg, &read_addr)) {
                    fprintf(stderr, "Error: invalid read address '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'w':
                action = ACTION_WRITE;
                if (!parse_u32_arg(optarg, &write_addr)) {
                    fprintf(stderr, "Error: invalid write address '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'R': if (action == ACTION_NONE) action = ACTION_RESET; else do_reset = true; break;
            case 's':
                if (!parse_u32_arg(optarg, &sector_addr)) {
                    fprintf(stderr, "Error: invalid sector address '%s'\n", optarg);
                    return 1;
                }
                has_sector = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    /* Handle write value as extra arg after -w ADDR */
    if (action == ACTION_WRITE && optind < argc) {
        if (!parse_u32_arg(argv[optind], &write_value)) {
            fprintf(stderr, "Error: invalid write value '%s'\n", argv[optind]);
            return 1;
        }
        optind++;
    }

    /* Handle read length as extra arg after -r ADDR */
    if (action == ACTION_READ && optind < argc) {
        if (!parse_u32_arg(argv[optind], (uint32_t *)&read_len)) {
            fprintf(stderr, "Error: invalid read length '%s'\n", argv[optind]);
            return 1;
        }
        if (read_len <= 0) read_len = 4;
        if (read_len > 65536) read_len = 65536;
        optind++;
    }

    /* List probes (no serial needed) */
    if (action == ACTION_LIST) {
        nrf_probe_t *probes = NULL;
        int count = 0;
        nrf_ocd_error_t err = nrf_probe_enum(&probes, &count);
        if (err != NRF_OCD_OK) {
            fprintf(stderr, "Failed to enumerate probes: %s\n", nrf_ocd_error_str(err));
            return 1;
        }
        if (count == 0) {
            printf("No CMSIS-DAP probes found.\n");
            nrf_probe_free_list(&probes, count);
            return 0;
        }
        printf("#   Probe/Board                                          Unique ID   Target\n");
        printf("--------------------------------------------------------------------------------\n");
        for (int i = 0; i < count; i++) {
            printf("%2d   %-54s %s\n", i, probes[i].product, probes[i].serial);
        }
        nrf_probe_free_list(&probes, count);
        return 0;
    }

    if (action == ACTION_NONE) {
        fprintf(stderr, "Error: no action specified\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Resolve serial from port if given */
    if (port_path != NULL && serial == NULL) {
        serial = nrf_serial_from_port(port_path);
        if (serial == NULL) {
            fprintf(stderr, "Error: could not determine probe serial from port %s\n", port_path);
            return 1;
        }
    }

    /* All other actions require a serial */
    if (!serial) {
        fprintf(stderr, "Error: probe serial required (-u <serial>)\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Find the probe */
    nrf_probe_t *probes = NULL;
    int count = 0;
    nrf_ocd_error_t err = nrf_probe_enum(&probes, &count);
    if (err != NRF_OCD_OK) {
        fprintf(stderr, "Failed to enumerate probes: %s\n", nrf_ocd_error_str(err));
        return 1;
    }

    nrf_probe_t *probe = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(probes[i].serial, serial) == 0) {
            probe = &probes[i];
            break;
        }
    }
    if (!probe) {
        fprintf(stderr, "Error: probe with serial '%s' not found\n", serial);
        nrf_probe_free_list(&probes, count);
        return 1;
    }

    const nrf_target_desc_t *target_desc = target_from_name(target);
    if (!target_desc) {
        fprintf(stderr, "Error: unknown target '%s' (use nrf54l15 or nrf54lm20a)\n",
                target ? target : "(null)");
        nrf_probe_free_list(&probes, count);
        return 1;
    }

    /* Initialize programmer */
    nrf_programmer_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.probe = probe;
    prog.target = target_desc;
    prog.flash_size = target_desc->flash_size;
    prog.auto_unlock = auto_unlock;
    prog.clock_hz = clock_hz;
    prog.connect_halt = (!connect_mode || strcmp(connect_mode, "halt") == 0);

    err = nrf_programmer_init(&prog, probe, target_desc);
    if (err != NRF_OCD_OK) {
        fprintf(stderr, "Failed to initialize: %s\n", nrf_ocd_error_str(err));
        nrf_programmer_close(&prog);
        nrf_probe_free_list(&probes, count);
        return 1;
    }

    switch (action) {
        case ACTION_INFO:
            err = nrf_programmer_info(&prog);
            if (err != NRF_OCD_OK) {
                fprintf(stderr, "Error: target mismatch or connection error\n");
                return 1;
            }
            break;

        case ACTION_ERASE:
            if (has_sector) {
                err = nrf_programmer_erase_sector(&prog, sector_addr);
            } else {
                err = nrf_programmer_erase(&prog);
            }
            break;

        case ACTION_FLASH:
            if (!hex_file) {
                fprintf(stderr, "Error: flash file required (-f <file.hex>)\n");
                err = NRF_OCD_ERR_HEX_PARSE;
            } else {
                nrf_hex_file_t hex;
                err = nrf_hex_parse(hex_file, &hex);
                if (err == NRF_OCD_OK) {
                    if (do_erase) {
                        err = nrf_programmer_erase(&prog);
                    }
                    if (err == NRF_OCD_OK) {
                        err = nrf_programmer_flash(&prog, &hex);
                    }
                    nrf_hex_free(&hex);
                }
            }
            break;

        case ACTION_READ:
            err = nrf_programmer_read(&prog, read_addr, read_len);
            break;

        case ACTION_WRITE:
            err = nrf_programmer_write_word(&prog, write_addr, write_value);
            break;

        case ACTION_RESET:
            err = nrf_programmer_reset(&prog);
            break;

        default:
            err = NRF_OCD_OK;
            break;
    }

    /* Clean up flash before reset (algorithm needs halted CPU) */
    nrf_flash_cleanup(&prog.flash);

    /* Reset if requested and action completed successfully */
    if (do_reset && !no_reset && err == NRF_OCD_OK &&
        (action == ACTION_FLASH || action == ACTION_ERASE)) {
        nrf_ocd_error_t rst_err = nrf_programmer_reset(&prog);
        if (rst_err == NRF_OCD_OK)
            NRF_INFO("Target reset after programming");
        else
            NRF_WARN("Post-flash reset failed: %s", nrf_ocd_error_str(rst_err));
    }

    nrf_dap_close(&prog.dap);
    nrf_probe_free_list(&probes, count);

    if (err != NRF_OCD_OK) {
        fprintf(stderr, "Error: %s\n", nrf_ocd_error_str(err));
        return 1;
    }

    return 0;
}

/* ==================== Programmer implementation ==================== */

nrf_ocd_error_t nrf_programmer_init(nrf_programmer_t *prog, nrf_probe_t *probe,
                                     const nrf_target_desc_t *target) {
    nrf_ocd_error_t err;

    if (!target)
        target = &nrf54l15_target;

    prog->probe = probe;
    prog->target = target;
    prog->flash_size = target->flash_size;

    /* Open the DAP interface */
    err = nrf_dap_open(&prog->dap, probe);
    if (err != NRF_OCD_OK)
        return err;

    /* Set clock (up to 8MHz for nRF54) */
    if (prog->clock_hz > 8000000) prog->clock_hz = 8000000;
    if (prog->clock_hz == 0) prog->clock_hz = 4000000;
    err = nrf_dap_set_clock(&prog->dap, prog->clock_hz);
    if (err != NRF_OCD_OK)
        goto fail;

    NRF_INFO("SWD clock: %u Hz", prog->clock_hz);

    /* Connect via SWD */
    err = nrf_swd_connect(&prog->dap);
    if (err != NRF_OCD_OK) {
        if (prog->auto_unlock) {
            /* SWD connect failed — device may be APPROTECT-locked.
             * Match pyOCD's --connect under-reset:
             * 1. Re-open probe (fresh USB connection)
             * 2. Assert nRESET via SWJ_PINS (probe open, DAP not connected)
             * 3. Do DAP_Connect (CPU held in reset, can't run APPROTECT code)
             * 4. Perform CTRL-AP mass erase
             * 5. Release nRESET
             * 6. Reconnect normally */
            NRF_INFO("SWD connect failed, trying connect-under-reset...");

            /* Step 1: Re-open probe (fresh USB connection) */
            nrf_dap_close(&prog->dap);
            usleep(200000);
            err = nrf_dap_open(&prog->dap, prog->probe);
            if (err != NRF_OCD_OK) goto fail;
            err = nrf_dap_set_clock(&prog->dap, prog->clock_hz);
            if (err != NRF_OCD_OK) goto fail;

            /* Step 2: Assert nRESET via SWJ_PINS (probe open, DAP not connected) */
            err = nrf_dap_swj_pins(&prog->dap, 0x80, 0, 0x00, 10000, NULL);
            if (err != NRF_OCD_OK) {
                NRF_ERR("Failed to assert nRESET");
                goto fail;
            }
            usleep(50000);  /* 50ms for reset to propagate */

            /* Step 3: DAP_Connect (CPU held in reset, can't run APPROTECT code) */
            err = nrf_dap_connect(&prog->dap, 0x01);  /* SWD mode */
            if (err != NRF_OCD_OK) {
                NRF_ERR("DAP_Connect under reset failed");
                goto fail;
            }

            /* Configure SWD for transfers (matches pyOCD connect flow) */
            {
                uint8_t scmd[2] = { 0x13, 0x00 };  /* SWD_Configure: turnaround=1 */
                nrf_probe_write(prog->probe, scmd, sizeof(scmd));
                uint8_t resp[64]; int rlen = 0;
                nrf_probe_read(prog->probe, resp, sizeof(resp), &rlen);
            }
            {
                uint8_t tcmd[7] = { 0x04, 2, 0x96, 0x00, 0x00, 0x00, 0x00 };  /* Transfer_Configure */
                nrf_probe_write(prog->probe, tcmd, sizeof(tcmd));
                uint8_t resp[64]; int rlen = 0;
                nrf_probe_read(prog->probe, resp, sizeof(resp), &rlen);
            }

            /* Step 4: CTRL-AP mass erase */
            NRF_INFO("Connected under reset, performing CTRL-AP mass erase...");
            err = nrf54_ctrl_mass_erase(&prog->dap);

            /* Step 5: Release nRESET */
            nrf_dap_swj_pins(&prog->dap, 0x80, 0, 0x80, 10000, NULL);

            if (err == NRF_OCD_OK) {
                /* Step 6: Reconnect clean after mass erase */
                nrf_dap_close(&prog->dap);
                usleep(100000);
                err = nrf_dap_open(&prog->dap, prog->probe);
                if (err != NRF_OCD_OK) goto fail;
                err = nrf_dap_set_clock(&prog->dap, prog->clock_hz);
                if (err != NRF_OCD_OK) goto fail;
                err = nrf_swd_connect(&prog->dap);
                if (err != NRF_OCD_OK) goto fail;
            } else {
                NRF_ERR("CTRL-AP unlock failed; try power-cycling the board.");
                goto fail;
            }
        } else {
            goto fail;
        }
    }

    /* Read DP IDCODE to verify connection */
    uint32_t idcode;
    err = nrf_dp_read(&prog->dap, DP_IDCODE, &idcode);
    if (err != NRF_OCD_OK)
        goto fail;

    NRF_INFO("Connected to device, DP IDCODE = 0x%08X", idcode);

    /* Check if device is locked (AP not enabled).
     * Only AP#0 (AHB-AP) CSW is checked. */
    prog->ap.ap_sel = 0;
    prog->ap.dap = &prog->dap;

    uint32_t csw;
    err = nrf_ap_read(&prog->dap, AP_CSW, &csw);
    if (err == NRF_OCD_OK) {
        prog->is_secure = !(csw & CSW_DEVICEEN);
    } else if (prog->auto_unlock) {
        NRF_WARN("AP#0 CSW read failed; attempting CTRL-AP unlock path");
        prog->is_secure = true;
    } else {
        NRF_ERR("Cannot read AP CSW — device may be locked. Use --auto-unlock.");
        goto fail;
    }

    if (prog->is_secure) {
        if (prog->auto_unlock) {
            NRF_INFO("Device is locked, performing mass erase via CTRL-AP...");
            err = nrf54_ctrl_mass_erase(&prog->dap);
            if (err != NRF_OCD_OK) {
                NRF_ERR("CTRL-AP mass erase failed: %s", nrf_ocd_error_str(err));
                goto fail;
            }

            /* Reconnect after mass erase */
            nrf_swd_disconnect(&prog->dap);
            nrf_dap_disconnect(&prog->dap);
            err = nrf_dap_set_clock(&prog->dap, prog->clock_hz);
            if (err != NRF_OCD_OK) goto fail;
            err = nrf_swd_connect(&prog->dap);
            if (err != NRF_OCD_OK) goto fail;

            prog->is_secure = false;
        } else {
            NRF_ERR("Device is locked. Use --auto-unlock to mass erase and unlock.");
            err = NRF_OCD_ERR_SWD_CONNECT;
            goto fail;
        }
    }

    /* Read AP IDR to confirm MEM-AP type */
    err = nrf_ap_read(&prog->dap, AP_IDR, &prog->ap.idr);
    if (err != NRF_OCD_OK)
        goto fail;

    NRF_INFO("AP#0 IDR = 0x%08X", prog->ap.idr);

    /* Initialize MEM-AP CSW from hardware */
    err = nrf_mem_init_csw(&prog->ap);
    if (err != NRF_OCD_OK) {
        NRF_ERR("Failed to read MEM-AP CSW: %s", nrf_ocd_error_str(err));
        goto fail;
    }

    /* Validate target: read UICR part number and verify it matches
     * the requested target. Prevents flashing wrong firmware to wrong chip. */
    {
        uint32_t part;
        err = nrf_mem_read32(&prog->ap, 0x00FFC31C, &part);
        if (err == NRF_OCD_OK) {
            bool ok = false;
            const char *expected = "unknown";

            if (target == &nrf54l15_target) {
                /* nRF54L15: part 0x54B15 */
                ok = (part == 0x00054B15);
                expected = "nRF54L15 (0x00054B15)";
            } else if (target == &nrf54lm20a_target) {
                /* nRF54LM20A: part 0x54BC20A */
                ok = (part == 0x054BC20A);
                expected = "nRF54LM20A (0x054BC20A)";
            }

            if (!ok) {
                NRF_ERR("Target mismatch: expected %s, but chip reports part 0x%08X",
                        expected, part);
                NRF_ERR("Use -t <target> to select the correct target type.");
                err = NRF_OCD_ERR_TARGET_MISMATCH;
                goto fail;
            }
            NRF_DBG("Target verified: part 0x%08X matches %s", part, target->name);
        } else {
            /* UICR read may fail if core is running/locked — warn but continue */
            NRF_WARN("Cannot read UICR part number (core may be running). "
                     "Target verification skipped.");
        }
    }

    /* ROM table (fixed for nRF54) */
    prog->ap.rom_addr = 0xE00FE000;
    prog->ap.has_rom_table = true;

    /* Set up flash context */
    prog->flash.ap = &prog->ap;
    prog->flash.target = target;
    prog->flash.flash_size = target->flash_size;
    prog->flash.prepared = false;
    prog->flash.inited = false;
    prog->flash.operation = 0;

    /* Halt core if requested (simple DHCSR write, no flash algo load).
     * NOTE: this must be done AFTER MEM-AP CSW init because the
     * DHCSR access goes through the MEM-AP. */
    if (prog->connect_halt) {
        NRF_DBG("Halting core per connect mode");
        uint32_t dhcsr;
        nrf_ocd_error_t halt_err = nrf_mem_read32(&prog->ap, 0xE000EDF0, &dhcsr);
        NRF_DBG("DHCSR=0x%08X", dhcsr);
        if (halt_err == NRF_OCD_OK && !(dhcsr & 0x00000002)) {
            halt_err = nrf_mem_write32(&prog->ap, 0xE000EDF0,
                                       0xA05F0003);  /* KEY | DEBUGEN | HALT */
            if (halt_err == NRF_OCD_OK)
                NRF_DBG("Core halted");
            else
                NRF_DBG("Core halt failed: %s", nrf_ocd_error_str(halt_err));
        }
    }

    if (err != NRF_OCD_OK) goto fail;
    return NRF_OCD_OK;

fail:
    nrf_dap_close(&prog->dap);
    return err;
}

void nrf_programmer_close(nrf_programmer_t *prog) {
    nrf_flash_cleanup(&prog->flash);
    nrf_dap_close(&prog->dap);
}

nrf_ocd_error_t nrf_programmer_erase(nrf_programmer_t *prog) {
    NRF_INFO("Erasing %s flash (0x%08X bytes)...", prog->target->name, prog->flash_size);
    return nrf_flash_erase_all(&prog->flash);
}

nrf_ocd_error_t nrf_programmer_erase_sector(nrf_programmer_t *prog, uint32_t addr) {
    NRF_INFO("Erasing sector at 0x%08X", addr);
    return nrf_flash_erase_sector(&prog->flash, addr);
}

static bool addr_in_main_flash(const nrf_programmer_t *prog, uint32_t addr) {
    return addr < prog->flash_size;
}

static bool addr_in_uicr(uint32_t addr) {
    return addr >= NRF54_UICR_START && addr < (NRF54_UICR_START + NRF54_UICR_SIZE);
}

static int programmable_span_len(const nrf_programmer_t *prog, uint32_t addr, int remaining) {
    uint32_t limit;
    if (addr_in_main_flash(prog, addr)) {
        limit = prog->flash_size;
    } else if (addr_in_uicr(addr)) {
        limit = NRF54_UICR_START + NRF54_UICR_SIZE;
    } else {
        return 0;
    }
    uint32_t available = limit - addr;
    if (available > (uint32_t)remaining)
        available = (uint32_t)remaining;
    return (int)available;
}

static int count_programmable_bytes(const nrf_programmer_t *prog, const nrf_hex_file_t *hex) {
    int total = 0;
    for (int i = 0; i < hex->count; i++) {
        uint32_t addr = hex->segments[i].addr;
        int remaining = hex->segments[i].len;
        while (remaining > 0) {
            int span = programmable_span_len(prog, addr, remaining);
            if (span <= 0) break;
            total += span;
            addr += (uint32_t)span;
            remaining -= span;
        }
    }
    return total;
}

nrf_ocd_error_t nrf_programmer_flash(nrf_programmer_t *prog, const nrf_hex_file_t *hex) {
    const uint32_t min_prog = prog->target->flash_algo.min_program_length;
    int total_bytes = count_programmable_bytes(prog, hex);
    /* 1KB program buffer for speed — matches sector size granularity */
    uint8_t program_buf[1024 + NRF54_MIN_PROG_LEN];

    if (total_bytes == 0) {
        NRF_WARN("No programmable data found in HEX file for target %s", prog->target->name);
        return NRF_OCD_OK;
    }

    NRF_INFO("Programming %d bytes from %d segment(s)...", total_bytes, hex->count);

    int bytes_programmed = 0;
    int sectors_erased = 0;
    uint32_t current_sector = 0xFFFFFFFF;

    for (int i = 0; i < hex->count; i++) {
        nrf_hex_segment_t *seg = &hex->segments[i];
        uint32_t addr = seg->addr;
        const uint8_t *data = seg->data;
        int remaining = seg->len;

        while (remaining > 0) {
            int span = programmable_span_len(prog, addr, remaining);
            if (span <= 0) {
                NRF_WARN("Segment at 0x%08X outside programmable regions, skipping %d byte(s)",
                         addr, remaining);
                break;
            }

            bool is_uicr = addr_in_uicr(addr);
            uint32_t sector_addr = addr & ~(NRF54_PAGE_SIZE - 1U);
            uint32_t sector_end = is_uicr
                                ? (NRF54_UICR_START + NRF54_UICR_SIZE)
                                : (sector_addr + NRF54_PAGE_SIZE);

            if (!is_uicr && sector_addr != current_sector) {
                nrf_ocd_error_t err = nrf_flash_erase_sector(&prog->flash, sector_addr);
                if (err != NRF_OCD_OK)
                    return err;
                current_sector = sector_addr;
                sectors_erased++;
            }

            uint32_t aligned_addr = addr & ~(min_prog - 1U);
            int prefix = (int)(addr - aligned_addr);
            int chunk = span;
            int sector_remaining = (int)(sector_end - addr);
            if (chunk > sector_remaining) chunk = sector_remaining;
            if (chunk > (int)(1024 - prefix)) chunk = (int)(1024 - prefix);

            int write_len = prefix + chunk;
            int aligned_len = (int)((write_len + min_prog - 1U) & ~(min_prog - 1U));
            memset(program_buf, 0xFF, (size_t)aligned_len);
            memcpy(program_buf + prefix, data, (size_t)chunk);

            NRF_DBG("Programming 0x%08X +%d byte(s) (%d payload)", aligned_addr, aligned_len, chunk);
            nrf_ocd_error_t err = nrf_flash_program_page(&prog->flash, aligned_addr,
                                                         program_buf, aligned_len);
            if (err != NRF_OCD_OK)
                return err;

            bytes_programmed += chunk;
            addr += (uint32_t)chunk;
            data += chunk;
            remaining -= chunk;

            int pct = (bytes_programmed * 100) / total_bytes;
            NRF_INFO("\r  Programming: %d/%d bytes (%d%%)", bytes_programmed, total_bytes, pct);
        }
    }

    NRF_INFO("\n  Done: %d bytes programmed, %d sectors erased", bytes_programmed, sectors_erased);
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_programmer_read(nrf_programmer_t *prog, uint32_t addr, int len) {
    uint32_t buf[64];

    printf("Reading %d bytes from 0x%08X:\n", len, addr);

    for (int offset = 0; offset < len; offset += 32) {
        int chunk = len - offset;
        if (chunk > 32) chunk = 32;
        int chunk_words = (chunk + 3) / 4;

        nrf_ocd_error_t err = nrf_mem_read_block32(&prog->ap, addr + offset,
                                                     buf, chunk_words);
        if (err != NRF_OCD_OK) {
            NRF_ERR("Read failed at 0x%08X: %s", addr + offset, nrf_ocd_error_str(err));
            return err;
        }

        /* Print hex dump */
        printf("0x%08X: ", addr + offset);
        for (int i = 0; i < chunk_words && i * 4 < chunk; i++) {
            printf("%08X ", buf[i]);
        }
        printf("\n");
    }

    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_programmer_write_word(nrf_programmer_t *prog, uint32_t addr, uint32_t value) {
    NRF_INFO("Writing 0x%08X to 0x%08X", value, addr);
    return nrf_mem_write32(&prog->ap, addr, value);
}

nrf_ocd_error_t nrf_programmer_reset(nrf_programmer_t *prog) {
    NRF_INFO("Resetting target...");

    /* Use AIRCR to request system reset */
    nrf_ocd_error_t err = nrf_mem_write32(&prog->ap, 0xE000ED0C,
                                           0x05FA0004);  /* VECTKEY | SYSRESETREQ */
    if (err != NRF_OCD_OK) {
        NRF_ERR("AIRCR write failed: %s", nrf_ocd_error_str(err));
        return err;
    }

    NRF_INFO("Reset requested");
    return NRF_OCD_OK;
}

nrf_ocd_error_t nrf_programmer_info(nrf_programmer_t *prog) {
    NRF_INFO("=== Device Info ===");
    NRF_INFO("Probe: %s %s (serial: %s)",
             prog->probe->vendor, prog->probe->product, prog->probe->serial);

    /* Read part info from UICR (may fault if core is running; that's OK) */
    uint32_t part, variant;
    nrf_ocd_error_t err = nrf_mem_read32(&prog->ap, 0x00FFC31C, &part);
    if (err == NRF_OCD_OK) {
        err = nrf_mem_read32(&prog->ap, 0x00FFC320, &variant);
        if (err == NRF_OCD_OK) {
            char variant_str[5];
            memcpy(variant_str, &variant, 4);
            variant_str[4] = '\0';
            NRF_INFO("Part: nRF%03X %s", part, variant_str);
            /* Detect board mismatch from USB probe descriptor */
            const char *expected = prog->target->name;
            const char *product = prog->probe->product;
            int is_lm20a = (strstr(product, "LM20") != NULL || strstr(product, "lm20") != NULL);
            int is_l15 = (strstr(product, "nRF54L15") != NULL || strstr(product, "nrf54l15") != NULL);
            if ((!strcmp(expected, "nrf54l15") && is_lm20a) ||
                (!strcmp(expected, "nrf54lm20a") && is_l15)) {
                NRF_ERR("BOARD MISMATCH: selected=%s, probe=%s", expected, product);
                NRF_ERR("Use -t %s for this board.", is_lm20a ? "nrf54lm20a" : "nrf54l15");
                return NRF_OCD_ERR_HEX_PARSE;
            }
        }
    } else {
        NRF_DBG("UICR part info not available (core may be running)");
    }

    NRF_INFO("Flash: %u bytes (%.1f MB)", prog->flash_size,
             prog->flash_size / 1048576.0);
    NRF_INFO("Target: %s", prog->target->name);
    NRF_INFO("Secure: %s", prog->is_secure ? "YES (locked)" : "NO");
    NRF_INFO("AP#0 IDR: 0x%08X", prog->ap.idr);
    NRF_INFO("ROM Table: 0x%08X", prog->ap.rom_addr);

    return NRF_OCD_OK;
}

static const char *nrf_serial_from_port(const char *port_path) {
    static char buf[64];
    if (!port_path) return NULL;
    const char *tty = port_path;
    const char *last_slash = strrchr(port_path, '/');
    if (last_slash) tty = last_slash + 1;
    if (*tty == '\0') return NULL;
    char syspath[512];
    int n = snprintf(syspath, sizeof(syspath),
                     "/sys/class/tty/%s/../../../serial", tty);
    if (n < 0 || (size_t)n >= sizeof(syspath)) return NULL;
    FILE *f = fopen(syspath, "r");
    if (!f) {
        n = snprintf(syspath, sizeof(syspath),
                     "/sys/class/tty/%s/device/../serial", tty);
        if (n < 0 || (size_t)n >= sizeof(syspath)) return NULL;
        f = fopen(syspath, "r");
    }
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return (len > 0) ? buf : NULL;
}
