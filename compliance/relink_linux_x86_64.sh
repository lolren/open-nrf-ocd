#!/usr/bin/env bash
# Relink the current nrf_ocd source tree with exact or modified libusb source.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBUSB_VERSION="1.0.27"
LIBUSB_SHA256="ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"
LIBUSB_SOURCE="${1:-${LIBUSB_SOURCE:-}}"
LIBUSB_ARCHIVE="${LIBUSB_ARCHIVE:-}"
ZIG_BIN="${ZIG:-zig}"
VERSION="${NRF_OCD_VERSION:-$(awk -F\" '/^#define NRF_OCD_VERSION / {print $2; exit}' "$ROOT/src/version.h")}"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/nrf_ocd-relink.XXXXXX")"
trap 'rm -rf "$BUILD_ROOT"' EXIT

if [ -z "$VERSION" ]; then
    echo "Unable to determine nrf_ocd version" >&2
    exit 2
fi

LIBUSB_DIR="$BUILD_ROOT/libusb-${LIBUSB_VERSION}"
if [ -n "$LIBUSB_SOURCE" ]; then
    test -f "$LIBUSB_SOURCE/configure"
    mkdir -p "$LIBUSB_DIR"
    cp -a "$LIBUSB_SOURCE/." "$LIBUSB_DIR/"
else
    if [ -z "$LIBUSB_ARCHIVE" ]; then
        for candidate in \
            "$ROOT/libusb-${LIBUSB_VERSION}.tar.bz2" \
            "$ROOT/release_dist/libusb-${LIBUSB_VERSION}.tar.bz2" \
            "${XDG_CACHE_HOME:-$HOME/.cache}/nrf_ocd/libusb-${LIBUSB_VERSION}.tar.bz2"; do
            if [ -f "$candidate" ]; then
                LIBUSB_ARCHIVE="$candidate"
                break
            fi
        done
    fi
    if [ -z "$LIBUSB_ARCHIVE" ] || [ ! -f "$LIBUSB_ARCHIVE" ]; then
        echo "libusb source not found; set LIBUSB_ARCHIVE or pass a modified source directory" >&2
        exit 2
    fi
    printf '%s  %s\n' "$LIBUSB_SHA256" "$LIBUSB_ARCHIVE" \
        | sha256sum --check --status
    tar -xjf "$LIBUSB_ARCHIVE" -C "$BUILD_ROOT"
fi

LIBUSB_BUILD="$BUILD_ROOT/libusb-build"
LIBUSB_INSTALL="$BUILD_ROOT/libusb-install"
mkdir -p "$LIBUSB_BUILD"
ZIG_CC="$ZIG_BIN cc -target x86_64-linux-gnu"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" show -s --format=%ct HEAD 2>/dev/null || printf '0')}"
export ZIG_LOCAL_CACHE_DIR="$BUILD_ROOT/zig-local-cache"
export ZIG_GLOBAL_CACHE_DIR="$BUILD_ROOT/zig-global-cache"

cd "$LIBUSB_BUILD"
"$LIBUSB_DIR/configure" --disable-udev --enable-static --disable-shared \
    --host=x86_64-linux-gnu CC="$ZIG_CC" AR="$ZIG_BIN ar" RANLIB="$ZIG_BIN ranlib" \
    CFLAGS="-Os -fPIC" --prefix="$LIBUSB_INSTALL"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
make install

cd "$ROOT"
OBJ_DIR="$BUILD_ROOT/nrf_ocd-obj"
mkdir -p "$OBJ_DIR" "$ROOT/relink-output"
SOURCES="log util hid_descriptor hex elf probe cmsis_dap swd dap target target_nrf54l target_nrf54lm20a flash flash_algo_nrf54l commander cli main hid_libusb"
CFLAGS="-std=c11 -Os -g0 -Wno-implicit-function-declaration -Wno-date-time -Wno-format -Wno-multichar -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DNRF_OCD_USE_LIBUSB=1 -DNRF_OCD_VERSION=\"$VERSION\" -Isrc -I$LIBUSB_INSTALL/include -I$LIBUSB_INSTALL/include/libusb-1.0"

for source in $SOURCES; do
    $ZIG_CC $CFLAGS -c "src/$source.c" -o "$OBJ_DIR/$source.o"
done

OUTPUT="$ROOT/relink-output/nrf_ocd-linux-amd64"
$ZIG_CC -o "$OUTPUT" "$OBJ_DIR"/*.o \
    "$LIBUSB_INSTALL/lib/libusb-1.0.a" -lpthread
chmod 0755 "$OUTPUT"
sha256sum "$OUTPUT"
