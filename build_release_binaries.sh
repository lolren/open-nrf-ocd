#!/usr/bin/env bash
# Build release binaries consumed by the Arduino core fallback downloader.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DIST="$ROOT/release_dist"
rm -rf "$DIST"
mkdir -p "$DIST"

echo "==> linux amd64, libusb bulk backend"
bash ./build_for_core.sh
strip -s build/bin/nrf_ocd || true
cp build/bin/nrf_ocd "$DIST/nrf_ocd-linux-amd64"
cp build/bin/nrf_ocd "$DIST/nrf_ocd-linux-x64"
cp build/bin/nrf_ocd "$DIST/nrf_ocd"

echo "==> linux arm64, HID backend"
make clean
make OS=linux USE_LIBUSB=0 CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar EXTRALDFLAGS=-static
aarch64-linux-gnu-strip -s build/bin/nrf_ocd || true
cp build/bin/nrf_ocd "$DIST/nrf_ocd-linux-arm64"

echo "==> linux armhf, HID backend"
make clean
make OS=linux USE_LIBUSB=0 CC=arm-linux-gnueabihf-gcc AR=arm-linux-gnueabihf-ar EXTRALDFLAGS=-static
arm-linux-gnueabihf-strip -s build/bin/nrf_ocd || true
cp build/bin/nrf_ocd "$DIST/nrf_ocd-linux-armhf"

echo "==> windows amd64, WinUSB/HID backend"
make clean
make OS=windows USE_LIBUSB=0 CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar EXTRALDFLAGS=-static
x86_64-w64-mingw32-strip -s build/bin/nrf_ocd.exe || true
cp build/bin/nrf_ocd.exe "$DIST/nrf_ocd-windows-amd64.exe"
cp build/bin/nrf_ocd.exe "$DIST/nrf_ocd-win64.exe"
cp build/bin/nrf_ocd.exe "$DIST/nrf_ocd.exe"

echo "==> windows i386, WinUSB/HID backend"
make clean
make OS=windows USE_LIBUSB=0 CC=i686-w64-mingw32-gcc AR=i686-w64-mingw32-ar EXTRALDFLAGS=-static
i686-w64-mingw32-strip -s build/bin/nrf_ocd.exe || true
cp build/bin/nrf_ocd.exe "$DIST/nrf_ocd-windows-i386.exe"

echo "==> release_dist"
sha256sum "$DIST"/*
file "$DIST"/*
