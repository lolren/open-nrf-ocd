#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LIBUSB_VERSION="1.0.27"
LIBUSB_SHA256="ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"
LIBUSB_ARCHIVE="$ROOT/third_party/libusb-${LIBUSB_VERSION}.tar.bz2"
LIBUSB_SOURCE="${1:-}"
ZIG_BIN="${ZIG:-zig}"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/nrf_ocd-relink.XXXXXX")"
trap 'rm -rf "$BUILD_ROOT"' EXIT

LIBUSB_DIR="$BUILD_ROOT/libusb-${LIBUSB_VERSION}"
if [ -n "$LIBUSB_SOURCE" ]; then
    test -f "$LIBUSB_SOURCE/configure"
    mkdir -p "$LIBUSB_DIR"
    cp -a "$LIBUSB_SOURCE/." "$LIBUSB_DIR/"
else
    printf '%s  %s\n' "$LIBUSB_SHA256" "$LIBUSB_ARCHIVE" \
        | sha256sum --check --status
    tar -xjf "$LIBUSB_ARCHIVE" -C "$BUILD_ROOT"
fi

LIBUSB_BUILD="$BUILD_ROOT/libusb-build"
LIBUSB_INSTALL="$BUILD_ROOT/libusb-install"
mkdir -p "$LIBUSB_BUILD"
ZIG_CC="$ZIG_BIN cc -target x86_64-linux-gnu"
export SOURCE_DATE_EPOCH=1783265844
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
SOURCES="log util hex elf probe cmsis_dap swd dap target target_nrf54l target_nrf54lm20a flash flash_algo_nrf54l commander cli main hid_libusb"
CFLAGS="-std=c11 -Os -g0 -Wno-implicit-function-declaration -Wno-date-time -Wno-format -Wno-multichar -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DNRF_OCD_USE_LIBUSB=1 -DNRF_OCD_VERSION=\"0.3.3\" -Isrc -I$LIBUSB_INSTALL/include -I$LIBUSB_INSTALL/include/libusb-1.0"

for source in $SOURCES; do
    $ZIG_CC $CFLAGS -c "src/$source.c" -o "$OBJ_DIR/$source.o"
done

OUTPUT="$ROOT/relink-output/nrf_ocd-linux-amd64"
$ZIG_CC -o "$OUTPUT" "$OBJ_DIR"/*.o "$LIBUSB_INSTALL/lib/libusb-1.0.a" -lpthread
chmod 0755 "$OUTPUT"
sha256sum "$OUTPUT"
