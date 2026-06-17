/* log.c - implementation of the leveled logger. */
#define _POSIX_C_SOURCE 200809L

#include "log.h"
#include "nrf_ocd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static log_level_t g_log_level = LOG_LEVEL_INFO;
static bool        g_log_quiet = false;
static bool        g_log_color = true;

void nrf_ocd_log_set_level(log_level_t level) { g_log_level = level; }
log_level_t nrf_ocd_log_get_level(void) { return g_log_level; }

void nrf_ocd_log_set_quiet(bool quiet) { g_log_quiet = quiet; }

static bool color_supported(void) {
    if (!g_log_color) return false;
#ifdef _WIN32
    return false;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

static const char *level_tag(log_level_t lvl) {
    switch (lvl) {
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARNING";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        case LOG_LEVEL_TRACE:   return "TRACE";
    }
    return "?";
}

static const char *level_color(log_level_t lvl) {
    switch (lvl) {
        case LOG_LEVEL_ERROR:   return "\x1b[31m";
        case LOG_LEVEL_WARNING: return "\x1b[33m";
        case LOG_LEVEL_INFO:    return "\x1b[32m";
        case LOG_LEVEL_DEBUG:   return "\x1b[36m";
        case LOG_LEVEL_TRACE:   return "\x1b[35m";
    }
    return "";
}

void nrf_ocd_log_emit(log_level_t level, const char *file, int line,
                      const char *fmt, ...) {
    if (g_log_quiet) return;
    if (level > g_log_level) return;

    const char *short_file = strrchr(file, '/');
    short_file = short_file ? short_file + 1 : file;
#ifdef _WIN32
    const char *alt = strrchr(short_file, '\\');
    if (alt) short_file = alt + 1;
#endif

#ifdef _WIN32
    time_t now = time(NULL);
    struct tm tm;
    localtime_s(&tm, &now);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
#endif

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

    FILE *out = (level <= LOG_LEVEL_WARNING) ? stderr : stdout;
    bool use_color = color_supported();
    if (use_color) {
        fprintf(out, "%s%s%s %s%7s%s %s:%d ",
                "\x1b[90m", timebuf, "\x1b[0m",
                level_color(level), level_tag(level), "\x1b[0m",
                short_file, line);
    } else {
        fprintf(out, "%s %-7s %s:%d ", timebuf, level_tag(level), short_file, line);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}
