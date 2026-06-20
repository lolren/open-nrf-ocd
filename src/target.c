/* target.c - target base class. */
#include "target.h"
#include "log.h"
#include "nrf_ocd.h"
#include "util.h"
#include "swd.h"
#include "dap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct target_ops target_ops_nrf54l15;
extern const struct target_ops target_ops_nrf54lm20a;

nrf_ocd_status_t target_open(target_t *t, hid_device_t *dev, target_type_t type) {
    if (!t || !dev) return NRF_OCD_ERR_INVALID_ARG;
    memset(t, 0, sizeof(*t));
    t->type = type;
    nrf_ocd_status_t st = cmsis_dap_open(&t->dap, dev);
    if (st != NRF_OCD_OK) return st;
    /* Switch to the right target ops. */
    switch (type) {
        case TARGET_NRF54L15:    t->ops = &target_ops_nrf54l15;    break;
        case TARGET_NRF54LM20A:  t->ops = &target_ops_nrf54lm20a;  break;
        default: return NRF_OCD_ERR_UNSUPPORTED;
    }
    return NRF_OCD_OK;
}

void target_close(target_t *t) {
    if (!t) return;
    cmsis_dap_close(&t->dap);
    memset(t, 0, sizeof(*t));
}

nrf_ocd_status_t target_init(target_t *t) {
    if (!t || !t->ops) return NRF_OCD_ERR_INVALID_ARG;
    if (t->ops->init) {
        nrf_ocd_status_t st = t->ops->init(t);
        if (st != NRF_OCD_OK) return st;
    }
    if (t->ops->check_security) return t->ops->check_security(t);
    return NRF_OCD_OK;
}

nrf_ocd_status_t target_mem_read(target_t *t, uint32_t addr, void *out, size_t n) {
    if (!t) return NRF_OCD_ERR_INVALID_ARG;
    return dap_mem_read(&t->dap, 0, addr, out, n);
}

nrf_ocd_status_t target_mem_write(target_t *t, uint32_t addr, const void *in, size_t n) {
    if (!t) return NRF_OCD_ERR_INVALID_ARG;
    return dap_mem_write(&t->dap, 0, addr, in, n);
}

nrf_ocd_status_t target_mem_write_u32(target_t *t, uint32_t addr, uint32_t value) {
    return target_mem_write(t, addr, &value, 4);
}

nrf_ocd_status_t target_mem_read_u32(target_t *t, uint32_t addr, uint32_t *value) {
    return target_mem_read(t, addr, value, 4);
}

const char *target_type_name(target_type_t type) {
    switch (type) {
        case TARGET_NRF54L15:   return "nrf54l15";
        case TARGET_NRF54LM20A: return "nrf54lm20a";
        default:               return "unknown";
    }
}

target_type_t target_type_from_string(const char *name) {
    if (!name) return TARGET_UNKNOWN;
    if (nrf_ocd_strcasecmp(name, "nrf54l15") == 0)    return TARGET_NRF54L15;
    if (nrf_ocd_strcasecmp(name, "nrf54l") == 0)      return TARGET_NRF54L15;
    if (nrf_ocd_strcasecmp(name, "nrf54lm20a") == 0)  return TARGET_NRF54LM20A;
    if (nrf_ocd_strcasecmp(name, "nrf54lm20b") == 0)  return TARGET_NRF54LM20A;
    return TARGET_UNKNOWN;
}
