#!/usr/bin/env bash
# Build nrf_ocd for the Arduino core.
# Requires: zig (snap install zig --classic --channel=edge)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Build static libusb without udev so the Linux binary remains self-contained
# apart from glibc/pthread.
echo "==> Building libusb..."
LIBUSB_VER=1.0.27
LIBUSB_DIR="/tmp/libusb-$LIBUSB_VER"
if [ ! -f "$LIBUSB_DIR/configure" ]; then
    curl -sL "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-${LIBUSB_VER}.tar.bz2" | tar xj -C /tmp
fi

ZIG_CC="zig cc -target x86_64-linux-gnu"
export ZIG_LOCAL_CACHE_DIR=/tmp/nrf_ocd_zig_local_cache
export ZIG_GLOBAL_CACHE_DIR=/tmp/nrf_ocd_zig_global_cache
rm -rf "$ZIG_LOCAL_CACHE_DIR" "$ZIG_GLOBAL_CACHE_DIR"
rm -rf /tmp/lb-build /tmp/lb-install
mkdir -p /tmp/lb-build
cd /tmp/lb-build
"$LIBUSB_DIR/configure" --disable-udev --enable-static --disable-shared \
    --host=x86_64-linux-gnu CC="$ZIG_CC" AR="zig ar" RANLIB="zig ranlib" \
    CFLAGS="-Os -fPIC" --prefix=/tmp/lb-install >/tmp/nrf_ocd_libusb_configure.log
make -j"$(nproc)" >/tmp/nrf_ocd_libusb_make.log
make install >/tmp/nrf_ocd_libusb_install.log

echo "==> Building nrf_ocd..."
cd "$ROOT"
rm -rf build
mkdir -p build/obj build/bin

SRC=src
CFLAGS="-std=c11 -Os -g0 -Wno-implicit-function-declaration -Wno-date-time -Wno-format -Wno-multichar -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DNRF_OCD_USE_LIBUSB=1 -I$SRC -I/tmp/lb-install/include -I/tmp/lb-install/include/libusb-1.0"

for f in log util hex elf probe cmsis_dap swd dap target target_nrf54l target_nrf54lm20a flash flash_algo_nrf54l commander cli main hid_libusb; do
  $ZIG_CC $CFLAGS -c "$SRC/$f.c" -o "build/obj/$f.o"
done

$ZIG_CC -o build/bin/nrf_ocd build/obj/*.o /tmp/lb-install/lib/libusb-1.0.a -lpthread

echo "==> Binary: build/bin/nrf_ocd ($(ls -lh build/bin/nrf_ocd | awk '{print $5}'))"
echo "==> Install: cp build/bin/nrf_ocd ~/.arduino15/packages/nrf54l15clean/hardware/nrf54l15clean/*/tools/nrf_ocd"
