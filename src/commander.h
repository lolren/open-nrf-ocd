/* commander.h - commander mode. */
#ifndef NRF_OCD_COMMANDER_H
#define NRF_OCD_COMMANDER_H

#include "target.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

nrf_ocd_status_t commander_run(target_t *t);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_COMMANDER_H */
