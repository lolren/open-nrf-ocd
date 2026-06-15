#!/bin/bash
# Cross-compile nrf_ocd for multiple architectures
# Usage: ./build_all.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CORE_FLAGS="-std=c11 -Os -I include -Wall -Wextra -Wno-unused-parameter"

HIDAPI_VER=0.14.0
LIBUSB_VER=1.0.27
HIDAPI_SRC="/tmp/hidapi-hidapi-${HIDAPI_VER}"
LIBUSB_SRC="/tmp/libusb-${LIBUSB_VER}"
HIDAPI_C="$HIDAPI_SRC/libusb/hid.c"
HIDAPI_H="$HIDAPI_SRC/hidapi/hidapi.h"

OUTDIR="$SCRIPT_DIR/release"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# ---------- helpers ----------
build_libusb_static() {
    local CC="$1" host="$2" out="$3"
    mkdir -p /tmp/lb-$host
    cd /tmp/lb-$host
    "$LIBUSB_SRC/configure" --host="$host" CC="$CC" \
        --disable-udev --enable-static --disable-shared \
        CFLAGS="-Os -fPIC" --prefix=/tmp/lb-install-$host >/dev/null 2>&1
    make -j$(nproc) >/dev/null 2>&1
    make install >/dev/null 2>&1
    cp /tmp/lb-install-$host/lib/libusb-1.0.a "$out/libusb-1.0.a"
}

build_target() {
    local name="$1" cc="$2" cflags="$3" linkflags="$4"
    local out="$OUTDIR/$name"
    mkdir -p "$out"

    echo "=== $name ==="

    # Compile hidapi-libusb from single source file
    "$cc" $cflags -c "$HIDAPI_C" -I "$HIDAPI_SRC/hidapi" -o "$out/hid.o" 2>&1 | tail -1
    ar rcs "$out/libhidapi.a" "$out/hid.o"

    # Build libusb from source
    case "$name" in
        linux-amd64)   build_libusb_static "$cc" x86_64-linux-gnu "$out" ;;
        linux-arm64)   build_libusb_static "$cc" aarch64-linux-gnu "$out" ;;
        linux-armhf)   build_libusb_static "$cc" arm-linux-gnueabihf "$out" ;;
    esac

    # Link nrf_ocd
    set +e
    "$cc" $cflags $CORE_FLAGS \
        src/nrf_ocd.c src/usb_backend.c src/cmsis_dap.c src/coresight.c \
        src/flash_algo.c src/intelhex.c src/log.c \
        "$out/libhidapi.a" "$out/libusb-1.0.a" \
        $linkflags -o "$out/nrf_ocd" 2>&1
    local rc=$?
    set -e

    if [ $rc -eq 0 ]; then
        strip "$out/nrf_ocd" 2>/dev/null || true
        local sz=$(stat -c%s "$out/nrf_ocd" 2>/dev/null || echo "?")
        echo "  -> $sz bytes  OK"
    else
        echo "  -> FAILED (rc=$rc)"
    fi
}

# ---------- download deps ----------
if [ ! -f "$HIDAPI_C" ]; then
    echo "Downloading hidapi..."
    curl -sL "https://github.com/libusb/hidapi/archive/refs/tags/hidapi-${HIDAPI_VER}.tar.gz" | tar xz -C /tmp
fi
if [ ! -d "$LIBUSB_SRC" ]; then
    echo "Downloading libusb..."
    curl -sL "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VER}/libusb-${LIBUSB_VER}.tar.bz2" | tar xj -C /tmp
fi

# ========== Linux amd64 ==========
build_target "linux-amd64" gcc "-Os" "-lpthread -ldl -ludev"

# ========== Linux ARM64 ==========
if command -v aarch64-linux-gnu-gcc &>/dev/null; then
    build_target "linux-arm64" aarch64-linux-gnu-gcc "-Os" "-lpthread -ldl -static"
fi

# ========== Linux ARM32 (armhf) ==========
if command -v arm-linux-gnueabihf-gcc &>/dev/null; then
    build_target "linux-armhf" arm-linux-gnueabihf-gcc "-Os -mfpu=vfp -mfloat-abi=hard" "-lpthread -ldl -static"
fi

# ========== Windows amd64 (zero-deps WinUSB backend) ==========
echo "=== windows-amd64 (zero deps) ==="
mkdir -p "$OUTDIR/windows-amd64"
x86_64-w64-mingw32-gcc $CORE_FLAGS -D_WIN32_WINNT=0x0601 \
    src/nrf_ocd.c src/usb_backend_win.c src/cmsis_dap.c src/coresight.c \
    src/flash_algo.c src/intelhex.c src/log.c \
    -lsetupapi -lhid -lwinusb -static \
    -o "$OUTDIR/windows-amd64/nrf_ocd.exe"
strip "$OUTDIR/windows-amd64/nrf_ocd.exe"
echo "  -> $(stat -c%s "$OUTDIR/windows-amd64/nrf_ocd.exe") bytes  OK"

# ========== Windows i386 (32-bit) ==========
if command -v i686-w64-mingw32-gcc &>/dev/null; then
    echo "=== windows-i386 (zero deps) ==="
    mkdir -p "$OUTDIR/windows-i386"
    i686-w64-mingw32-gcc $CORE_FLAGS -D_WIN32_WINNT=0x0601 \
        src/nrf_ocd.c src/usb_backend_win.c src/cmsis_dap.c src/coresight.c \
        src/flash_algo.c src/intelhex.c src/log.c \
        -lsetupapi -lhid -lwinusb -static \
        -o "$OUTDIR/windows-i386/nrf_ocd.exe"
    strip "$OUTDIR/windows-i386/nrf_ocd.exe" 2>/dev/null || true
    echo "  -> $(stat -c%s "$OUTDIR/windows-i386/nrf_ocd.exe") bytes  OK"
fi

# ========== Summary ==========
echo ""
echo "================================================"
echo "  BUILD COMPLETE"
echo "================================================"
find "$OUTDIR" -type f -name "nrf_ocd*" -exec ls -lh {} \; 2>/dev/null
