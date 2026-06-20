# nrf_ocd — C reimplementation of pyOCD for nRF54L15 / nRF54LM20A

`nrf_ocd` is a from-scratch C reimplementation of pyOCD targeted at the
Seeed XIAO nRF54L15 (`VID=0x2886 PID=0x0066`) and Seeed XIAO nRF54LM20A
(`VID=0x2886 PID=0x0068`) boards. It supports Linux, macOS and Windows.

## Status

| Feature                                  | Status     | Notes |
| ---------------------------------------- | ---------- | ----- |
| Probe enumeration                        | ✅ Works   | Reads serial, product, manufacturer |
| DAP_Info / DAP_Connect / SWD             | ✅ Works   | |
| DP/AP register read/write                | ✅ Works   | |
| Memory read/write via AHB-AP             | ✅ Works   | 32-bit, auto-increment |
| Flash mass-erase via CTRL-AP             | ✅ Works   | |
| Flash programming via flash algorithm    | ✅ Works   | ~3.3 kB/s |
| Flash verify (read-back)                  | ✅ Works   | Word-by-word verification |
| Commander REPL                           | ✅ Works   | Interactive memory debug |
| ELF + Intel HEX loader                   | ✅ Works   | |
| Both targets (L15 & LM20A)               | ✅ Works   | Auto-selects flash controller |
| libusb bulk backend                      | ✅ Works   | Recommended for speed |

## Building

### Prerequisites

- **Linux**: `libusb-1.0` (for bulk backend, recommended)
- **macOS**: IOKit framework (included with Xcode)
- **Windows**: hid.dll (included with Windows)

### Build with libusb bulk backend (recommended)

```sh
make USE_LIBUSB=1 CC=/usr/bin/gcc-13
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/bin/nrf_ocd list
```

### Build with HID backend (no dependencies)

```sh
make
./build/bin/nrf_ocd list
```

The bulk backend uses USB bulk transfers (CMSIS-DAP v2) which are
significantly faster than HID. On Seeed XIAO nRF54 boards the HID
interface has truncated response issues, so the bulk backend is
strongly recommended.

## Usage

```sh
# List CMSIS-DAP probes
nrf_ocd list

# Show target info (reads UICR.PARTNO)
nrf_ocd -t nrf54l15 -u 761FDE87 info

# Program flash with chip erase and reset
nrf_ocd -t nrf54l15 -u 761FDE87 -e chip -R load firmware.hex

# Mass erase the chip
nrf_ocd -t nrf54l15 -u 761FDE87 erase

# Reset target
nrf_ocd -t nrf54l15 -u 761FDE87 reset

# Read 16 bytes from UICR.PARTNO
nrf_ocd -t nrf54l15 -u 761FDE87 read 0x00FFC31C 16

# Write to RAM (4 bytes)
nrf_ocd -t nrf54l15 -u 761FDE87 write 0x20000000 DEADBEEF

# Interactive memory REPL
nrf_ocd -t nrf54l15 -u 761FDE87 commander
```

### Example output

```
$ nrf_ocd -t nrf54l15 -u 761FDE87 load firmware.hex
Loaded 1 segment(s), 22764 bytes total
Erasing chip...
Erase complete
Programming 22764 byte(s) in 1 segment(s)
  ... 6144 / 22764 bytes (27.0%)
  ... 12288 / 22764 bytes (54.0%)
  ... 18432 / 22764 bytes (81.0%)
Programmed 22764 bytes in 1 segment(s)
Upload complete
```

## Targets

| Target        | Board              | VID:PID       | Flash     | RAM    | Flash Controller |
|---------------|--------------------|---------------|-----------|--------|------------------|
| `nrf54l15`    | XIAO nRF54L15      | `2886:0066`   | 1.5 MB    | 256 KB | NVMC @ 0x5004B000 |
| `nrf54lm20a`  | XIAO nRF54LM20A    | `2886:0068`   | 2036 KB   | 512 KB | RRAMC @ 0x5004E000 |

Both share the same CTRL-AP-based mass-erase sequence and flash
algorithm. The flash controller is selected automatically based on
the target type.

## Architecture

Flash programming uses a **flash algorithm** — position-independent ARM
Thumb code loaded into the target's RAM and executed via core register
manipulation (DCRSR/DCRDR). This is the same approach pyOCD uses and
is required because the nRF54L15's NVMC does not support direct AHB-AP
writes to flash addresses.

### Key files

```
src/
  main.c                - Entry point
  cli.c                 - CLI (pyOCD-compatible syntax)
  probe.c               - USB probe enumeration
  hid.h                 - Portable HID abstraction
  hid_linux.c           - /dev/hidraw backend
  hid_macos.c           - IOKit HID backend (macOS)
  hid_windows.c         - hid.dll backend (Windows)
  hid_libusb.c          - libusb bulk backend (USE_LIBUSB=1)
  cmsis_dap.c           - CMSIS-DAP v1/v2 protocol
  swd.c                 - SWD line-level helpers
  dap.c                 - DP/AP register + memory access
  target.c              - Target base class
  target_nrf54l.c       - nRF54L15 target
  target_nrf54lm20a.c   - nRF54LM20A target
  flash.c               - Flash programming engine
  flash_algo_nrf54l.c   - Flash algorithm (runs on target CPU)
  flash_algo_nrf54l.h   - Flash algorithm metadata
  commander.c           - pyOCD-style commander REPL
  hex.c                 - Intel HEX parser
  elf.c                 - ELF parser
  log.c                 - Leveled logger
  util.c                - Endian, hex dump, time helpers
tests/
  test_hex.c            - HEX parser tests
  test_elf.c            - ELF parser tests
  test_target.c         - Target type tests
```

## Tests

```sh
make test
```

Runs unit tests for the Intel HEX parser, ELF parser and target type
lookup.

## Arduino Core

This tool is the default uploader for the
[nRF54L15 Clean Arduino Core](https://github.com/lolren/nrf54-arduino-core).
Install the core and use **Tools → Upload Method → nRF OCD (Native)**
to flash via nrf_ocd without any Python dependency.

```bash
arduino-cli core update-index && arduino-cli core install nrf54l15clean:nrf54l15clean@0.9.194
nrf_ocd -p /dev/ttyACM0 -t nrf54lm20a -e chip -R load sketch.hex
```

## Known limitations

- **Speed**: Flash programming achieves ~3.3 kB/s (limited by per-page
  function call overhead in the flash algorithm). pyOCD achieves higher
  throughput through double-buffering optimizations.
- **DAP_TransferBlock on HID**: The SAMD11 USB bridge truncates HID
  responses to 63 bytes. The bulk backend (USE_LIBUSB=1) avoids this
  issue. When using HID, the code tolerates truncated responses.
