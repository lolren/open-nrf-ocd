/* commander.c - simple commander mode.
 *
 * Provides a line-oriented REPL that lets the user read/write memory
 * directly via the target's DAP, and run init sequences similar to
 * pyOCD commander.
 *
 * Commands (subset of pyOCD):
 *   help                 - show command list
 *   quit / exit          - leave the REPL
 *   initdp               - init SWD / JTAG debug port
 *   readdp <reg>         - read DP register
 *   writedp <reg> <val>  - write DP register
 *   readap <ap> <reg>    - read AP register
 *   writeap <ap> <reg> <val>
 *   read <addr>          - read 4 bytes from memory (hex address)
 *   read8 <addr> <n>     - read n bytes (hex)
 *   read32 <addr> <n>    - read n*4 bytes (hex)
 *   write <addr> <val>   - write 4 bytes to memory
 *   write8 <addr> <hex>  - write raw bytes
 *   reset                - reset the target
 *   halt                 - halt the target
 *   resume               - resume the target
 */
#include "target.h"
#include "cmsis_dap.h"
#include "dap.h"
#include "log.h"
#include "nrf_ocd.h"
#include "swd.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tokenize(char *line, char *tokens[], int max_tokens) {
    int n = 0;
    char *p = line;
    while (*p && n < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return n;
}

static nrf_ocd_status_t cmd_readap(target_t *t, int argc, char **argv) {
    if (argc < 2) {
        LOG_ERROR("Usage: readap <ap> <reg>");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t ap = (uint32_t)strtoul(argv[0], NULL, 0);
    uint32_t reg = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t value = 0;
    nrf_ocd_status_t st = dap_ap_read(&t->dap, (uint8_t)ap, (uint8_t)reg, &value);
    if (st != NRF_OCD_OK) return st;
    printf("0x%08x\n", value);
    return NRF_OCD_OK;
}

static nrf_ocd_status_t cmd_writeap(target_t *t, int argc, char **argv) {
    if (argc < 3) {
        LOG_ERROR("Usage: writeap <ap> <reg> <value>");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t ap = (uint32_t)strtoul(argv[0], NULL, 0);
    uint32_t reg = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t value = (uint32_t)strtoul(argv[2], NULL, 0);
    return dap_ap_write(&t->dap, (uint8_t)ap, (uint8_t)reg, value);
}

static nrf_ocd_status_t cmd_readdp(target_t *t, int argc, char **argv) {
    if (argc < 1) {
        LOG_ERROR("Usage: readdp <reg>");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t reg = (uint32_t)strtoul(argv[0], NULL, 0);
    uint32_t value = 0;
    nrf_ocd_status_t st = dap_dp_read(&t->dap, (uint8_t)reg, &value);
    if (st != NRF_OCD_OK) return st;
    printf("0x%08x\n", value);
    return NRF_OCD_OK;
}

static nrf_ocd_status_t cmd_writedp(target_t *t, int argc, char **argv) {
    if (argc < 2) {
        LOG_ERROR("Usage: writedp <reg> <value>");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t reg = (uint32_t)strtoul(argv[0], NULL, 0);
    uint32_t value = (uint32_t)strtoul(argv[1], NULL, 0);
    return dap_dp_write(&t->dap, (uint8_t)reg, value);
}

static nrf_ocd_status_t cmd_read(target_t *t, int argc, char **argv) {
    if (argc < 1) {
        LOG_ERROR("Usage: read <addr> [count]");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t addr = (uint32_t)strtoul(argv[0], NULL, 0);
    size_t count = argc >= 2 ? (size_t)strtoul(argv[1], NULL, 0) : 1;
    uint8_t *buf = (uint8_t *)malloc(count);
    if (!buf) return NRF_OCD_ERR_NO_MEM;
    nrf_ocd_status_t st = target_mem_read(t, addr, buf, count);
    if (st != NRF_OCD_OK) {
        free(buf);
        return st;
    }
    nrf_ocd_hex_dump(buf, count, addr, stdout);
    free(buf);
    return NRF_OCD_OK;
}

static nrf_ocd_status_t cmd_write(target_t *t, int argc, char **argv) {
    if (argc < 2) {
        LOG_ERROR("Usage: write <addr> <hexbytes>");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    uint32_t addr = (uint32_t)strtoul(argv[0], NULL, 0);
    const char *hex = argv[1];
    size_t hlen = strlen(hex);
    if (hlen & 1) {
        LOG_ERROR("hex string must have even length");
        return NRF_OCD_ERR_INVALID_ARG;
    }
    size_t n = hlen / 2;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return NRF_OCD_ERR_NO_MEM;
    for (size_t i = 0; i < n; i++) {
        char tmp[3] = { hex[2*i], hex[2*i+1], 0 };
        buf[i] = (uint8_t)strtoul(tmp, NULL, 16);
    }
    nrf_ocd_status_t st = target_mem_write(t, addr, buf, n);
    free(buf);
    return st;
}

static void print_help(void) {
    printf("nrf_ocd commander commands:\n"
           "  help                       - show this message\n"
           "  quit / exit                - leave the REPL\n"
           "  readdp <reg>               - read DP register (hex or decimal)\n"
           "  writedp <reg> <val>        - write DP register\n"
           "  readap <ap> <reg>          - read AP register\n"
           "  writeap <ap> <reg> <val>   - write AP register\n"
           "  read <addr> [count]        - read memory (default 1 byte)\n"
           "  write <addr> <hex>         - write memory bytes\n"
           "  reset                      - reset the target\n"
           "  halt                       - halt the target\n"
           "  resume                     - resume the target\n");
}

nrf_ocd_status_t commander_run(target_t *t) {
    if (!t) return NRF_OCD_ERR_INVALID_ARG;
    char line[512];
    print_help();
    while (1) {
        printf("nrf_ocd> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        nrf_ocd_str_rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *tokens[8] = {0};
        int argc = tokenize(line, tokens, 8);
        if (argc == 0) continue;
        nrf_ocd_status_t st = NRF_OCD_OK;
        if (nrf_ocd_strcasecmp(tokens[0], "help") == 0) {
            print_help();
        } else if (nrf_ocd_strcasecmp(tokens[0], "quit") == 0
                || nrf_ocd_strcasecmp(tokens[0], "exit") == 0) {
            break;
        } else if (nrf_ocd_strcasecmp(tokens[0], "readdp") == 0) {
            st = cmd_readdp(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "writedp") == 0) {
            st = cmd_writedp(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "readap") == 0) {
            st = cmd_readap(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "writeap") == 0) {
            st = cmd_writeap(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "read") == 0) {
            st = cmd_read(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "write") == 0) {
            st = cmd_write(t, argc - 1, tokens + 1);
        } else if (nrf_ocd_strcasecmp(tokens[0], "reset") == 0) {
            if (t->ops && t->ops->reset) st = t->ops->reset(t, TARGET_RESET_DEFAULT);
        } else if (nrf_ocd_strcasecmp(tokens[0], "halt") == 0) {
            if (t->ops && t->ops->halt) st = t->ops->halt(t);
        } else if (nrf_ocd_strcasecmp(tokens[0], "resume") == 0) {
            if (t->ops && t->ops->resume) st = t->ops->resume(t);
        } else {
            LOG_ERROR("Unknown command: %s", tokens[0]);
        }
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Command failed: %s", nrf_ocd_strerror(st));
        }
    }
    return NRF_OCD_OK;
}
