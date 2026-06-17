/* cli.c - command-line interface.
 *
 * Layout:
 *   nrf_ocd list                       - list probes
 *   nrf_ocd info [-t target] [-u uid]  - show target info
 *   nrf_ocd load [-t target] [-u uid] [-e chip|auto|none] [--auto-unlock]
 *                [-R] [--no-reset] [--verify|--no-verify] [--halt]
 *                <file>...
 *   nrf_ocd erase [-t target] [-u uid]
 *   nrf_ocd reset [-t target] [-u uid] [-M halt|default]
 *   nrf_ocd commander [-t target] [-u uid] [-f freq]
 *   nrf_ocd read [-t target] [-u uid] <addr> [count]
 *   nrf_ocd write [-t target] [-u uid] <addr> <hex>
 *
 * Global options (-t, -u, -f, -v, -q) are accepted anywhere before
 * the first positional argument; subcommand-specific options
 * (-e, --auto-unlock, -R, etc.) are also accepted at the global
 * position for ergonomic compatibility with pyOCD. The pyOCD-style
 * invocation:
 *
 *   nrf_ocd -t nrf54l15 -u 761FDE87 --auto-unlock -e chip -f file.hex -R
 *
 * just works.
 */
#include "cli.h"
#include "commander.h"
#include "elf.h"
#include "flash.h"
#include "hex.h"
#include "log.h"
#include "nrf_ocd.h"
#include "probe.h"
#include "target.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- helpers ------------------------------------------------------------ */
static int parse_int(const char *s, int dflt) {
    if (!s) return dflt;
    if (nrf_ocd_strncasecmp(s, "0x", 2) == 0) return (int)strtoul(s + 2, NULL, 16);
    if (s[0] == '0' && s[1] != '\0')           return (int)strtoul(s + 1, NULL, 8);
    return (int)strtoul(s, NULL, 10);
}

static uint32_t parse_u32(const char *s, uint32_t dflt) {
    if (!s) return dflt;
    if (nrf_ocd_strncasecmp(s, "0x", 2) == 0) return (uint32_t)strtoul(s + 2, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 0);
}

/* ----- cmd: list ---------------------------------------------------------- */
static int cmd_list(void) {
    probe_info_t probes[32];
    size_t n = 0;
    nrf_ocd_status_t st = probe_list(probes, 32, &n);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("probe_list failed: %s", nrf_ocd_strerror(st));
        return 1;
    }
    if (n == 0) {
        LOG_WARNING("No CMSIS-DAP probes found");
        return 0;
    }
    printf("  #   Probe/Board                                          Unique ID    Target\n");
    printf("--------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < n; i++) {
        printf("  %-3zu %-50s %-12s %s\n",
               i, probes[i].product, probes[i].serial,
               target_type_name(target_type_from_string("nrf54l15")));
    }
    return 0;
}

/* ----- shared context ----------------------------------------------------- */
typedef struct {
    target_type_t   target;
    char            uid[64];
    unsigned        index;
    int             freq_khz;
    flash_options_t flash_opts;
    bool            auto_unlock;
    bool            no_reset;
    bool            reset_after;
    bool            halt;
} cli_ctx_t;

static void cli_ctx_init(cli_ctx_t *c) {
    memset(c, 0, sizeof(*c));
    c->target  = TARGET_NRF54L15;
    c->freq_khz = 1000;
    c->flash_opts.erase  = FLASH_ERASE_NONE;
    c->flash_opts.verify = true;
}

static nrf_ocd_status_t open_target(cli_ctx_t *ctx, target_t *t) {
    hid_device_t *dev = NULL;
    probe_info_t  info;
    memset(&info, 0, sizeof(info));
    nrf_ocd_status_t st = probe_open(&info, &dev, ctx->uid[0] ? ctx->uid : NULL, ctx->index);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Could not open probe: %s", nrf_ocd_strerror(st));
        return st;
    }
    printf("  Probe: %s [%s] (%s) at %s\n",
             info.product, info.serial, info.manufacturer, info.path);
    st = target_open(t, dev, ctx->target);
    if (st != NRF_OCD_OK) {
        hid_close(dev);
        return st;
    }
    st = target_init(t);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("target_init failed: %s", nrf_ocd_strerror(st));
        target_close(t);
        return st;
    }
    if (t->ops && t->ops->read_part_info) {
        (void)t->ops->read_part_info(t);
    }
    return NRF_OCD_OK;
}

/* ----- cmd: info --------------------------------------------------------- */
static int cmd_info(cli_ctx_t *ctx) {
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;
    target_close(&t);
    return 0;
}

/* ----- cmd: load (program flash) ----------------------------------------- */
static int cmd_load(cli_ctx_t *ctx, int argc, char **argv) {
    if (argc < 1) {
        LOG_ERROR("Usage: nrf_ocd load <file>...");
        return 1;
    }
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;

    hex_image_t img;
    hex_image_init(&img);
    for (int i = 0; i < argc; i++) {
        const char *path = argv[i];
        if (nrf_ocd_str_endswith(path, ".hex") || nrf_ocd_str_endswith(path, ".ihx")) {
            st = hex_image_load(&img, path);
        } else if (nrf_ocd_str_endswith(path, ".elf")) {
            elf_info_t info;
            st = elf_load(path, &img, &info);
            if (st == NRF_OCD_OK) {
                LOG_INFO("ELF entry point: 0x%08x", info.entry);
            }
        } else {
            LOG_ERROR("Unknown file format: %s", path);
            st = NRF_OCD_ERR_FILE_FORMAT;
        }
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Failed to load %s: %s", path, nrf_ocd_strerror(st));
            hex_image_free(&img);
            target_close(&t);
            return 1;
        }
    }
    if (img.count == 0) {
        LOG_ERROR("No data to program");
        hex_image_free(&img);
        target_close(&t);
        return 1;
    }
    size_t total = 0;
    hex_image_total_size(&img, &total);
    printf("  Loaded %zu KB (%zu segment(s))\n", total / 1024, img.count);

    st = flash_write_image(&t, &img, &ctx->flash_opts);
    hex_image_free(&img);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Flash programming failed: %s", nrf_ocd_strerror(st));
        target_close(&t);
        return 1;
    }
    if (ctx->reset_after && !ctx->no_reset) {
        if (t.ops && t.ops->reset) {
            st = t.ops->reset(&t, TARGET_RESET_DEFAULT);
            if (st != NRF_OCD_OK) LOG_WARNING("Reset failed: %s", nrf_ocd_strerror(st));
        }
    }
    target_close(&t);
    printf("  Upload complete\n");
    return 0;
}

/* ----- cmd: erase -------------------------------------------------------- */
static int cmd_erase(cli_ctx_t *ctx) {
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;
    st = flash_chip_erase(&t);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Erase failed: %s", nrf_ocd_strerror(st));
        target_close(&t);
        return 1;
    }
    printf("  Chip erase complete\n");
    target_close(&t);
    return 0;
}

/* ----- cmd: reset -------------------------------------------------------- */
static int cmd_reset(cli_ctx_t *ctx) {
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;
    if (t.ops && t.ops->reset) {
        st = t.ops->reset(&t, ctx->halt ? TARGET_RESET_HALT : TARGET_RESET_DEFAULT);
        if (st != NRF_OCD_OK) {
            LOG_ERROR("Reset failed: %s", nrf_ocd_strerror(st));
            target_close(&t);
            return 1;
        }
    }
    target_close(&t);
    printf("  Target reset\n");
    return 0;
}

/* ----- cmd: commander ---------------------------------------------------- */
static int cmd_commander(cli_ctx_t *ctx) {
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;
    st = commander_run(&t);
    target_close(&t);
    return (st == NRF_OCD_OK) ? 0 : 1;
}

/* ----- cmd: read / write memory ------------------------------------------ */
static int cmd_read_mem(cli_ctx_t *ctx, int argc, char **argv) {
    if (argc < 1) {
        LOG_ERROR("Usage: nrf_ocd read <addr> [count]");
        return 1;
    }
    uint32_t addr = parse_u32(argv[0], 0);
    size_t count = argc > 1 ? (size_t)parse_int(argv[1], 16) : 16;
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) return 1;
    uint8_t *buf = (uint8_t *)malloc(count);
    if (!buf) { target_close(&t); return 1; }
    st = target_mem_read(&t, addr, buf, count);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Memory read failed: %s", nrf_ocd_strerror(st));
        free(buf);
        target_close(&t);
        return 1;
    }
    nrf_ocd_hex_dump(buf, count, addr, stdout);
    free(buf);
    target_close(&t);
    return 0;
}

static int cmd_write_mem(cli_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        LOG_ERROR("Usage: nrf_ocd write <addr> <hex>");
        return 1;
    }
    uint32_t addr = parse_u32(argv[0], 0);
    const char *hex = argv[1];
    size_t hlen = strlen(hex);
    if (hlen & 1) {
        LOG_ERROR("hex string must have even length");
        return 1;
    }
    size_t n = hlen / 2;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return 1;
    for (size_t i = 0; i < n; i++) {
        char tmp[3] = { hex[2*i], hex[2*i+1], 0 };
        buf[i] = (uint8_t)strtoul(tmp, NULL, 16);
    }
    target_t t;
    nrf_ocd_status_t st = open_target(ctx, &t);
    if (st != NRF_OCD_OK) { free(buf); return 1; }
    st = target_mem_write(&t, addr, buf, n);
    free(buf);
    if (st != NRF_OCD_OK) {
        LOG_ERROR("Memory write failed: %s", nrf_ocd_strerror(st));
        target_close(&t);
        return 1;
    }
    target_close(&t);
    return 0;
}

/* ----- usage ------------------------------------------------------------- */
static void print_usage(FILE *f) {
    fprintf(f,
        "Usage: nrf_ocd [global options] <command> [command options]\n"
        "\n"
        "Commands:\n"
        "  list                          List CMSIS-DAP probes\n"
        "  info                          Connect and show target info\n"
        "  load <file>...                Program flash from HEX/ELF file(s)\n"
        "  erase                         Mass erase the chip\n"
        "  reset                         Reset the target\n"
        "  commander                     Interactive memory REPL\n"
        "  read <addr> [count]           Read bytes from target memory\n"
        "  write <addr> <hex>            Write bytes to target memory\n"
        "\n"
        "Global options:\n"
        "  -u, --uid <serial>            Probe unique ID (e.g. 761FDE87)\n"
        "  -t, --target <name>           Target type: nrf54l15, nrf54lm20a\n"
        "      --index <n>               Probe index when no UID given\n"
        "  -f, --frequency <khz>         SWD clock in kHz (default 1000)\n"
        "  -v, --verbose                 Increase log level (-vv, -vvv)\n"
        "  -q, --quiet                   Decrease log level\n"
        "  -h, --help                    Show this help\n"
        "\n"
        "Load/erase options (also accepted at the global position):\n"
        "  -e, --erase <mode>            Erase mode: auto, chip, sector, none\n"
        "      --auto-unlock             Mass erase on locked target\n"
        "  -R, --reset                   Reset target after programming\n"
        "      --no-reset                Do not reset target after programming\n"
        "      --verify                  Verify after programming (default on)\n"
        "      --no-verify               Skip verify\n"
        "      --halt                    Halt target before programming\n"
        "\n"
        "Examples:\n"
        "  nrf_ocd list\n"
        "  nrf_ocd -t nrf54l15 -u 761FDE87 --auto-unlock -e chip -f firmware.hex -R\n"
        "  nrf_ocd -t nrf54l15 -u 761FDE87 erase\n"
        "  nrf_ocd -t nrf54l15 -u 761FDE87 read 0x00FFC31C 16\n"
        "\n");
}

/* ----- argument parser --------------------------------------------------- */
static int apply_long_option(cli_ctx_t *ctx, int val, const char *arg) {
    switch (val) {
        case 'u': strncpy(ctx->uid, arg, sizeof(ctx->uid) - 1); return 0;
        case 't': {
            target_type_t ty = target_type_from_string(arg);
            if (ty == TARGET_UNKNOWN) {
                LOG_ERROR("Unknown target: %s", arg);
                return 1;
            }
            ctx->target = ty;
            return 0;
        }
        case 1001: ctx->index = (unsigned)parse_int(arg, 0); return 0;
        case 'f':
            if (arg && (nrf_ocd_str_endswith(arg, ".hex")
                     || nrf_ocd_str_endswith(arg, ".ihx")
                     || nrf_ocd_str_endswith(arg, ".elf")
                     || nrf_ocd_str_endswith(arg, ".bin"))) {
                /* Filename at global scope - leave it for `load`. */
                return -1; /* sentinel meaning "leave arg in argv" */
            }
            ctx->freq_khz = parse_int(arg, 1000);
            return 0;
        case 'e':
            if (nrf_ocd_strcasecmp(arg, "auto") == 0)        ctx->flash_opts.erase = FLASH_ERASE_AUTO;
            else if (nrf_ocd_strcasecmp(arg, "chip") == 0)  ctx->flash_opts.erase = FLASH_ERASE_CHIP;
            else if (nrf_ocd_strcasecmp(arg, "sector") == 0) ctx->flash_opts.erase = FLASH_ERASE_SECTOR;
            else if (nrf_ocd_strcasecmp(arg, "none") == 0)  ctx->flash_opts.erase = FLASH_ERASE_NONE;
            else { LOG_ERROR("Unknown erase mode: %s", arg); return 1; }
            return 0;
        case 'R': ctx->reset_after = true; return 0;
        case 2001: ctx->auto_unlock = true; return 0;
        case 2002: ctx->no_reset = true; return 0;
        case 2003: ctx->flash_opts.verify = true; return 0;
        case 2004: ctx->flash_opts.verify = false; return 0;
        case 2005: ctx->halt = true; return 0;
        default:  return 1;
    }
}

static int parse_opts(int argc, char **argv, cli_ctx_t *ctx) {
    static struct option long_opts[] = {
        {"uid",         required_argument, 0, 'u'},
        {"target",      required_argument, 0, 't'},
        {"index",       required_argument, 0, 1001},
        {"frequency",   required_argument, 0, 'f'},
        {"verbose",     no_argument,       0, 'v'},
        {"quiet",       no_argument,       0, 'q'},
        {"help",        no_argument,       0, 'h'},
        {"auto-unlock", no_argument,       0, 2001},
        {"reset",       no_argument,       0, 'R'},
        {"no-reset",    no_argument,       0, 2002},
        {"verify",      no_argument,       0, 2003},
        {"no-verify",   no_argument,       0, 2004},
        {"halt",        no_argument,       0, 2005},
        {"erase",       required_argument, 0, 'e'},
        {0, 0, 0, 0},
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+u:t:f:vqhe:R", long_opts, NULL)) != -1) {
        switch (c) {
            case 'u': case 't': case 1001: case 'f': case 'e':
            case 'R': case 2001: case 2002: case 2003: case 2004: case 2005: {
                int r = apply_long_option(ctx, c, optarg);
                if (r > 0) return 1;
                if (r < 0) {
                    /* -f <file>: rewind optind so the file arg is left in argv. */
                    optind -= 2;
                }
                break;
            }
            case 'v': {
                log_level_t lvl = nrf_ocd_log_get_level();
                if (lvl < LOG_LEVEL_TRACE) nrf_ocd_log_set_level(lvl + 1);
                break;
            }
            case 'q': {
                log_level_t lvl = nrf_ocd_log_get_level();
                if (lvl > LOG_LEVEL_ERROR) nrf_ocd_log_set_level(lvl - 1);
                break;
            }
            case 'h': print_usage(stdout); exit(0);
            default:  LOG_ERROR("Unknown option"); print_usage(stderr); return 1;
        }
    }
    return 0;
}

int cli_run(int argc, char **argv) {
    cli_ctx_t ctx;
    cli_ctx_init(&ctx);
    if (argc < 2) { print_usage(stderr); return 1; }
    if (parse_opts(argc, argv, &ctx) != 0) return 1;

    /* Whatever non-option args remain after parse_opts() are the
     * subcommand and its arguments. */
    if (optind >= argc) { print_usage(stderr); return 1; }
    const char *cmd = argv[optind];
    int cmd_argc = argc - optind - 1;
    char **cmd_argv = argv + optind + 1;

    if (nrf_ocd_strcasecmp(cmd, "list") == 0) {
        return cmd_list();
    } else if (nrf_ocd_strcasecmp(cmd, "info") == 0) {
        return cmd_info(&ctx);
    } else if (nrf_ocd_strcasecmp(cmd, "load") == 0) {
        return cmd_load(&ctx, cmd_argc, cmd_argv);
    } else if (nrf_ocd_strcasecmp(cmd, "flash") == 0) {
        /* Alias for load. */
        return cmd_load(&ctx, cmd_argc, cmd_argv);
    } else if (nrf_ocd_strcasecmp(cmd, "erase") == 0) {
        return cmd_erase(&ctx);
    } else if (nrf_ocd_strcasecmp(cmd, "reset") == 0) {
        return cmd_reset(&ctx);
    } else if (nrf_ocd_strcasecmp(cmd, "commander") == 0
            || nrf_ocd_strcasecmp(cmd, "cmd") == 0) {
        return cmd_commander(&ctx);
    } else if (nrf_ocd_strcasecmp(cmd, "read") == 0) {
        return cmd_read_mem(&ctx, cmd_argc, cmd_argv);
    } else if (nrf_ocd_strcasecmp(cmd, "write") == 0) {
        return cmd_write_mem(&ctx, cmd_argc, cmd_argv);
    } else if (nrf_ocd_strcasecmp(cmd, "help") == 0
            || nrf_ocd_strcasecmp(cmd, "-h") == 0
            || nrf_ocd_strcasecmp(cmd, "--help") == 0) {
        print_usage(stdout);
        return 0;
    } else if (nrf_ocd_str_endswith(cmd, ".hex") || nrf_ocd_str_endswith(cmd, ".ihx")
            || nrf_ocd_str_endswith(cmd, ".elf") || nrf_ocd_str_endswith(cmd, ".bin")) {
        /* Bare filename - default to load. */
        return cmd_load(&ctx, cmd_argc + 1, argv + optind);
    } else {
        LOG_ERROR("Unknown command: %s", cmd);
        print_usage(stderr);
        return 1;
    }
}