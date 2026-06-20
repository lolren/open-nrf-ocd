/* log.h - tiny leveled logger for nrf_ocd
 *
 * Mirrors pyOCD's logger levels (ERROR / WARNING / INFO / DEBUG / TRACE) so
 * that CLI output looks familiar to anyone used to pyocd. Logging goes to
 * stderr by default; the level is configurable via nrf_ocd_log_set_level().
 */
#ifndef NRF_OCD_LOG_H
#define NRF_OCD_LOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARNING = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4,
} log_level_t;

void nrf_ocd_log_set_level(log_level_t level);
log_level_t nrf_ocd_log_get_level(void);

void nrf_ocd_log_set_quiet(bool quiet);

/* Internal: format a single log line. All public macros below funnel through
 * nrf_ocd_log_emit(). Avoid calling it directly so that file/line info
 * stays accurate. */
#if defined(__GNUC__) || defined(__clang__)
#define NRF_OCD_PRINTF_FORMAT(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define NRF_OCD_PRINTF_FORMAT(fmt_index, first_arg)
#endif

void nrf_ocd_log_emit(log_level_t level, const char *file, int line,
                      const char *fmt, ...)
    NRF_OCD_PRINTF_FORMAT(4, 5);

#define LOG_ERROR(...)   nrf_ocd_log_emit(LOG_LEVEL_ERROR,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARNING(...) nrf_ocd_log_emit(LOG_LEVEL_WARNING, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)    nrf_ocd_log_emit(LOG_LEVEL_INFO,    __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...)   nrf_ocd_log_emit(LOG_LEVEL_DEBUG,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_TRACE(...)   nrf_ocd_log_emit(LOG_LEVEL_TRACE,   __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* NRF_OCD_LOG_H */
