/* cli.h - command-line interface. */
#ifndef NRF_OCD_CLI_H
#define NRF_OCD_CLI_H

#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

int cli_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_CLI_H */
