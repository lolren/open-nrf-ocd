/* flash.h - flash programming engine. */
#ifndef NRF_OCD_FLASH_H
#define NRF_OCD_FLASH_H

#include "nrf_ocd.h"
#include "target.h"
#include "hex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLASH_ERASE_NONE = 0,
    FLASH_ERASE_AUTO,
    FLASH_ERASE_CHIP,
    FLASH_ERASE_SECTOR,
} flash_erase_mode_t;

typedef struct {
    flash_erase_mode_t erase;
    bool               reset_after;
    bool               verify;
    /* If true, halt the target before programming (so the running image
     * doesn't disturb us). The nRF54L programming writes to flash
     * regardless of CPU state, so we typically do not need this. */
    bool               halt;
} flash_options_t;

nrf_ocd_status_t flash_chip_erase(target_t *t);
nrf_ocd_status_t flash_write_image(target_t *t, const hex_image_t *img,
                                   const flash_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_FLASH_H */
