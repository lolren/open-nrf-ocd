/* elf.h - minimal ELF32 / ELF64 reader.
 *
 * We only care about program headers of type PT_LOAD that occupy FLASH /
 * RAM. We also propagate the entry point so the commander can decide
 * whether the firmware image looks plausible.
 */
#ifndef NRF_OCD_ELF_H
#define NRF_OCD_ELF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "hex.h"
#include "nrf_ocd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t entry;
    bool     is_64;
} elf_info_t;

nrf_ocd_status_t elf_load(const char *path, hex_image_t *img, elf_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_ELF_H */
