#include "version.h"
/* main.c - nrf_ocd entry point. */
#include "cli.h"
#include "hid.h"
#include "log.h"
#include "nrf_ocd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    LOG_INFO("nrf_ocd %s (built " __DATE__ " " __TIME__ ")", NRF_OCD_VERSION);
    nrf_ocd_status_t st = hid_init();
    if (st != NRF_OCD_OK) {
        LOG_ERROR("hid_init failed: %s", nrf_ocd_strerror(st));
        return 1;
    }
    int rc = cli_run(argc, argv);
    hid_shutdown();
    return rc;
}
