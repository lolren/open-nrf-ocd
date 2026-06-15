# nrf_ocd - Native C CMSIS-DAP flash programmer for nRF54L15 / nRF54LM20A
#
# Cross-platform build: Linux, macOS, Windows (MinGW cross-compile).
#
# Linux:
#   cmake -B build -S . && cmake --build build
#
# macOS:
#   brew install hidapi libusb
#   cmake -B build -S . && cmake --build build
#
# Windows (cross-compile from Linux):
#   cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake
#   cmake --build build_win

cmake_minimum_required(VERSION 3.16)
project(nrf_ocd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============== Platform detection ==============

if(WIN32 OR MINGW)
    # ---------------- Windows (native or MinGW cross-compile) ----------------
    set(SOURCES
        src/nrf_ocd.c
        src/usb_backend_win.c
        src/cmsis_dap.c
        src/coresight.c
        src/flash_algo.c
        src/intelhex.c
        src/log.c
    )
    add_executable(nrf_ocd ${SOURCES})
    target_include_directories(nrf_ocd PRIVATE include)
    target_link_libraries(nrf_ocd PRIVATE -lsetupapi -lhid -lwinusb -static)
    target_compile_options(nrf_ocd PRIVATE -Wall -Wextra)
    target_compile_definitions(nrf_ocd PRIVATE _WIN32_WINNT=0x0601)
    set_target_properties(nrf_ocd PROPERTIES SUFFIX ".exe")

elseif(APPLE)
    # ---------------- macOS ----------------
    # Use Homebrew-installed hidapi and libusb.
    # Cross-platform usb_backend.c works on macOS (hidapi IOHIDManager + libusb IOKit).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIDAPI REQUIRED hidapi)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    set(SOURCES
        src/nrf_ocd.c
        src/usb_backend.c
        src/cmsis_dap.c
        src/coresight.c
        src/flash_algo.c
        src/intelhex.c
        src/log.c
    )
    add_executable(nrf_ocd ${SOURCES})
    target_include_directories(nrf_ocd PRIVATE include
        ${HIDAPI_INCLUDE_DIRS} ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(nrf_ocd PRIVATE
        ${HIDAPI_LIBRARIES} ${LIBUSB_LIBRARIES})
    target_compile_options(nrf_ocd PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS nrf_ocd DESTINATION /usr/local/bin)

else()
    # ---------------- Linux ----------------
    # Force system gcc to avoid conda glibc incompatibility.
    find_program(SYSTEM_GCC /usr/bin/gcc)
    if(SYSTEM_GCC)
        set(CMAKE_C_COMPILER ${SYSTEM_GCC})
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIDAPI REQUIRED hidapi-libusb)

    # Try to find static hidapi; fall back to shared.
    find_library(HIDAPI_STATIC
        NAMES libhidapi-libusb.a hidapi-libusb
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
        NO_DEFAULT_PATH)
    find_library(LIBUSB_SHARED
        NAMES libusb-1.0.so
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
        NO_DEFAULT_PATH)

    set(SOURCES
        src/nrf_ocd.c
        src/usb_backend.c
        src/cmsis_dap.c
        src/coresight.c
        src/flash_algo.c
        src/intelhex.c
        src/log.c
    )
    add_executable(nrf_ocd ${SOURCES})
    target_include_directories(nrf_ocd PRIVATE include ${HIDAPI_INCLUDE_DIRS})

    if(HIDAPI_STATIC AND LIBUSB_SHARED)
        target_link_libraries(nrf_ocd PRIVATE ${HIDAPI_STATIC} ${LIBUSB_SHARED}
            -lpthread -ldl)
    else()
        target_link_libraries(nrf_ocd PRIVATE ${HIDAPI_LIBRARIES}
            -lusb-1.0 -lpthread -ldl)
    endif()

    target_compile_options(nrf_ocd PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS nrf_ocd DESTINATION /usr/local/bin)
endif()
