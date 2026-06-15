/*
 * platform.h - Cross-platform compatibility (POSIX <-> Windows)
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
#include <windows.h>

/* usleep(microseconds) -> Windows Sleep(milliseconds) */
static inline void usleep(unsigned int usec) {
    unsigned int ms = usec / 1000;
    if (ms == 0 && usec > 0) ms = 1;
    Sleep(ms);
}

#else
#include <unistd.h>
#endif

#endif /* PLATFORM_H */
