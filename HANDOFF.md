# nrf_ocd Handoff Document

## Current State (2026-06-16)

**Repository:** https://github.com/lolren/open-nrf-ocd
**Branch:** master
**Latest commit:** `690e730` fix: add SWD configure commands to auto-unlock path after DAP_Connect
**Latest tag:** `v1.0.4`

## What Works

- **Normal flash flow:** 100/100 consecutive flash cycles verified
- **Targets:** nRF54L15 and nRF54LM20A
- **Probes:** XIAO nRF54 CMSIS-DAP (2886:0066/0068), picotool/RP2040 (2e8a:000c)
- **Platforms:** Linux (amd64/arm64/armhf), Windows (amd64/i386)
- **Flash content verified:** `0x20026000` at 0x0 (correct stack pointer)
- **Board blinks after reset**

## Known Issues

### 1. `--auto-unlock` CTRL-AP path hangs/fails (ACTIVE ISSUE)

**Symptom:** When SWD connect fails (APPROTECT-locked board), the auto-unlock path triggers but hangs at "Connected under reset, performing CTRL-AP mass erase..." or fails with "Transfer error".

**Reproduction:**
```bash
# On an APPROTECT-locked board:
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 --auto-unlock -i
```

**Current code path (src/nrf_ocd.c ~line 390):**
```c
/* Step 1: Re-open probe */
nrf_dap_close(&prog->dap);
err = nrf_dap_open(&prog->dap, prog->probe);

/* Step 2: DAP_Connect with mode 0x03 (SWD with reset) */
err = nrf_dap_connect(&prog->dap, 0x03);  /* FAILS on XIAO SAMD11 */

/* OR alternatively with SWJ_PINS: */
err = nrf_dap_swj_pins(&prog->dap, 0x80, 0, 0x00, 10000, NULL);  /* assert nRESET */
err = nrf_dap_connect(&prog->dap, 0x01);  /* SWD mode */

/* Step 3: CTRL-AP mass erase */
err = nrf54_ctrl_mass_erase(&prog->dap);  /* HANGS HERE */
```

**Root cause:** The CTRL-AP access requires SWD transfers to work, but the target is in NOACK state. The DAP_Transfer commands return NOACK (0x07) because:
- The target is APPROTECT-locked
- The debug port is locked
- The target doesn't respond to SWD commands

**What was tried (all failed):**

1. **DAP_Connect mode 0x03 (connect-under-reset):**
   - Result: `ERROR: DAP_Connect failed`
   - Reason: XIAO SAMD11 bridge doesn't support mode 0x03

2. **SWJ_PINS before DAP_Connect:**
   - Result: DAP_Connect succeeds, but CTRL-AP access fails with NOACK
   - Reason: Target still in NOACK state after DAP_Connect

3. **SWD configure commands after DAP_Connect:**
   - Result: Commands sent but target still returns NOACK
   - Reason: SWD protocol not initialized, target not responding

4. **SWD line reset after DAP_Connect:**
   - Result: SWD line reset fails (DP_IDCODE read fails)
   - Reason: Target in NOACK state, can't read DP registers

**pyOCD comparison:**

pyOCD's `--connect under-reset` flow (from `coresight_target.py`):
```python
# pre_connect(): assert nRESET before DAP connect
self.dp.assert_reset(True)  # SWJ_PINS(0, 0x80)

# Then normal DAP connect
self.probe.connect()

# Then SWD configure
self.probe.swd_configure()

# Then perform_halt_on_connect()
# Then post_connect(): deassert reset
self.dp.assert_reset(False)  # SWJ_PINS(0x80, 0x80)
```

pyOCD's CTRL-AP access (from `target_nRF54L.py`):
```python
self.ctrl_ap = self.dp.aps[CTRL_AP_NUM]
if self.ctrl_ap.idr != CTRL_IDR_EXPECTED:
    LOG.error("bad CTRL-AP IDR")
```

**Key differences to investigate:**
1. pyOCD uses `SWJ_PINS(0, 0x80)` for assert, we use `SWJ_PINS(0x80, 0x00)`
2. pyOCD does SWD configure BEFORE CTRL-AP access
3. pyOCD may have different timing/delays

**To investigate:**
1. Add verbose debug logging to see exact DAP_Transfer request/response bytes
2. Compare byte-for-byte with pyOCD's USB traffic (use `strace -e trace=write` on pyOCD)
3. Check if the XIAO SAMD11 bridge has firmware bugs affecting CTRL-AP access
4. Try with a different probe (e.g., nRF52840 DK) to isolate bridge issues
5. Try `SWJ_PINS(0, 0x80)` instead of `SWJ_PINS(0x80, 0x00)` to match pyOCD exactly

### 2. Intermittent `erase_all` hangs

**Symptom:** Occasionally, the `erase_all` flash function hangs — the core doesn't halt after the BKPT instruction.

**Root cause:** Unknown — possibly NVMC state issue or flash algorithm timing.

**Mitigation:** Added system reset (AIRCR) before `erase_all` to ensure NVMC is in clean state. This reduced but didn't eliminate the issue.

## Build Instructions

### Linux (native)
```bash
# Install dependencies
sudo apt install cmake pkg-config libhidapi-libusb-dev libusb-1.0-0-dev

# Build
cmake -B build -S . && cmake --build build

# Test
sudo ./build/nrf_ocd -t nrf54l15 -u 761FDE87 -e -f firmware.hex
```

### Windows (cross-compile from Linux)
```bash
# Install MinGW
sudo apt install gcc-mingw-w64-x86-64

# Build
cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake && cmake --build build_win

# Test (on Windows)
build_win/nrf_ocd.exe -t nrf54l15 -u 761FDE87 -e -f firmware.hex
```

### macOS
```bash
# Install dependencies
brew install hidapi libusb cmake

# Build
cmake -B build -S . && cmake --build build
```

## Test Commands

### Basic flash
```bash
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 -e -f firmware.hex
```

### With reset after programming
```bash
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 -e -f firmware.hex -R
```

### Info only
```bash
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 -i
```

### Memory read
```bash
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 -r 0x0 16
```

### Auto-unlock (for locked boards)
```bash
sudo ./nrf_ocd-linux-amd64 -t nrf54l15 -u 761FDE87 --auto-unlock -e -f firmware.hex
```

### Stress test (100 cycles)
```bash
fails=0
for i in $(seq 1 100); do
    sudo timeout 60 ./nrf_ocd-linux-amd4 -t nrf54l15 -u 761FDE87 -e -f firmware.hex 2>&1 | tail -1
    [ $? -eq 0 ] || ((fails++))
done
echo "Fails: $fails/100"
```

## Connected Probes

| Board | Serial | VID:PID | Transport |
|-------|--------|---------|-----------|
| XIAO nRF54L15 | 761FDE87 | 2886:0066 | v2 libusb bulk @ 4MHz |
| XIAO nRF54LM20A | 3377B9D6 | 2886:0068 | v2 libusb bulk @ 4MHz |

## Key Files

| File | Purpose |
|------|---------|
| `src/nrf_ocd.c` | CLI + programmer orchestration |
| `src/cmsis_dap.c` | CMSIS-DAP protocol + DAP_Transfer |
| `src/coresight.c` | SWD connect, DP/AP, CTRL-AP |
| `src/flash_algo.c` | Flash algorithms, call_flash_function |
| `src/usb_backend.c` | USB transport (Linux/macOS) |
| `src/usb_backend_win.c` | USB transport (Windows) |
| `include/nrf_ocd.h` | Public API header |

## Critical Context

- **MEM-AP CSW is read-only on nRF54:** Don't try to write it. Use cached value with correct HPROT bits (0x3000052).
- **DP_SELECT caching:** Avoid redundant register writes by caching the last DP_SELECT value.
- **WAIT retry fix:** Changed `static int wait_count` to local `wait_retries` and added hard limit that returns error.
- **erased_all flag:** Re-init flash algorithm after `erase_all` before `erase_sector`.
- **HID v1 fallback:** Auto-fallback when v2 bulk endpoints return 0 bytes (SAMD11 bridge issue).
- **Mid-session USB recovery:** Auto-reconnect when v2 bulk transfers fail.

## pyOCD Reference

pyOCD is installed at:
```
/home/lolren/pinokio/bin/miniconda/lib/python3.10/site-packages/pyocd/
```

Key files for comparison:
- `pyocd/coresight/dap.py` — DAP layer
- `pyocd/coresight/ap.py` — AP access
- `pyocd/probe/cmsis_dap_probe.py` — CMSIS-DAP probe
- `pyocd/probe/pydapaccess/cmsis_dap_core.py` — CMSIS-DAP protocol
- `pyocd/target/family/target_nRF54L.py` — nRF54 target

## Next Steps

1. **Fix `--auto-unlock` CTRL-AP path:** Compare byte-for-byte with pyOCD's USB traffic
2. **Test with locked board:** Need an APPROTECT-locked board to test the auto-unlock path
3. **Test with different probe:** Try nRF52840 DK to isolate XIAO SAMD11 bridge issues
4. **Add ELF support:** Beyond Intel HEX
5. **Add flash verify:** CRC check after programming
6. **Add macOS binaries:** Requires building on a Mac
