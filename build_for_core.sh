#!/usr/bin/env bash
# Build nrf_ocd for the Arduino core.
# Requires: Zig, curl, tar, make, and sha256sum.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Build static libusb without udev so the Linux binary remains self-contained
# apart from the system C runtime.
LIBUSB_VER="${LIBUSB_VER:-1.0.27}"
LIBUSB_SHA256="ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VER}/libusb-${LIBUSB_VER}.tar.bz2"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/nrf_ocd"
LIBUSB_ARCHIVE="${LIBUSB_ARCHIVE:-$CACHE_DIR/libusb-${LIBUSB_VER}.tar.bz2}"
ZIG_BIN="${ZIG:-zig}"
VERSION="${VERSION:-0.3.4}"

mkdir -p "$(dirname "$LIBUSB_ARCHIVE")"
if [ ! -f "$LIBUSB_ARCHIVE" ]; then
    echo "==> Downloading libusb $LIBUSB_VER..."
    curl --fail --location --retry 3 --output "$LIBUSB_ARCHIVE.part" "$LIBUSB_URL"
    mv "$LIBUSB_ARCHIVE.part" "$LIBUSB_ARCHIVE"
fi
printf '%s  %s\n' "$LIBUSB_SHA256" "$LIBUSB_ARCHIVE" | sha256sum --check --status

BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/nrf_ocd-build.XXXXXX")"
trap 'rm -rf "$BUILD_ROOT"' EXIT
LIBUSB_DIR="$BUILD_ROOT/libusb-$LIBUSB_VER"
LIBUSB_BUILD="$BUILD_ROOT/libusb-build"
LIBUSB_INSTALL="$BUILD_ROOT/libusb-install"
mkdir -p "$LIBUSB_BUILD"
tar -xjf "$LIBUSB_ARCHIVE" -C "$BUILD_ROOT"

ZIG_CC="$ZIG_BIN cc -target x86_64-linux-gnu"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD 2>/dev/null || printf '0')}"
export ZIG_LOCAL_CACHE_DIR="$BUILD_ROOT/zig-local-cache"
export ZIG_GLOBAL_CACHE_DIR="$BUILD_ROOT/zig-global-cache"

echo "==> Building libusb $LIBUSB_VER..."
cd "$LIBUSB_BUILD"
"$LIBUSB_DIR/configure" --disable-udev --enable-static --disable-shared \
    --host=x86_64-linux-gnu CC="$ZIG_CC" AR="$ZIG_BIN ar" RANLIB="$ZIG_BIN ranlib" \
    CFLAGS="-Os -fPIC" --prefix="$LIBUSB_INSTALL" \
    >"$BUILD_ROOT/libusb-configure.log"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')" \
    >"$BUILD_ROOT/libusb-make.log"
make install >"$BUILD_ROOT/libusb-install.log"

echo "==> Building nrf_ocd..."
cd "$ROOT"
rm -rf build
mkdir -p build/obj build/bin

SRC=src
CFLAGS="-std=c11 -Os -g0 -Wno-implicit-function-declaration -Wno-date-time -Wno-format -Wno-multichar -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DNRF_OCD_USE_LIBUSB=1 -DNRF_OCD_VERSION=\"$VERSION\" -I$SRC -I$LIBUSB_INSTALL/include -I$LIBUSB_INSTALL/include/libusb-1.0"
SOURCES="log util hex elf probe cmsis_dap swd dap target target_nrf54l target_nrf54lm20a flash flash_algo_nrf54l commander cli main hid_libusb"

for f in $SOURCES; do
    $ZIG_CC $CFLAGS -c "$SRC/$f.c" -o "build/obj/$f.o"
done

$ZIG_CC -o build/bin/nrf_ocd build/obj/*.o "$LIBUSB_INSTALL/lib/libusb-1.0.a" -lpthread

echo "==> Binary: build/bin/nrf_ocd ($(ls -lh build/bin/nrf_ocd | awk '{print $5}'))"
echo "==> Install: cp build/bin/nrf_ocd ~/.arduino15/packages/nrf54l15clean/hardware/nrf54l15clean/*/tools/nrf_ocd"
