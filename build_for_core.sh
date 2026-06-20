#!/bin/bash
# Build nrf_ocd for the Arduino core
# Run this after cloning: bash build_for_core.sh
set -euo pipefail

echo "==> Installing dependencies..."
sudo apt-get install -y libusb-1.0-0-dev build-essential 2>/dev/null || true

echo "==> Building nrf_ocd..."
CC=${CC:-gcc}
USE_LIBUSB=1 make clean 2>/dev/null || true
CC="$CC" USE_LIBUSB=1 make -j$(nproc)

echo "==> Binary: build/bin/nrf_ocd"
ls -lh build/bin/nrf_ocd

echo ""
echo "==> To install in Arduino core:"
echo "  cp build/bin/nrf_ocd ~/.arduino15/packages/nrf54l15clean/hardware/nrf54l15clean/VERSION/tools/nrf_ocd"
echo ""
echo "==> Or run directly:"
echo "  ./build/bin/nrf_ocd -p /dev/ttyACM1 -t nrf54lm20a -e chip -R load sketch.hex"
