# nrf_ocd — C reimplementation of pyOCD for nRF54L15 / nRF54LM20A

`nrf_ocd` is a compact C implementation of CMSIS-DAP programming targeted at the
Seeed XIAO nRF54L15 (`VID=0x2886 PID=0x0066`) and Seeed XIAO nRF54LM20A
(`VID=0x2886 PID=0x0068`) boards. Its nRF54L target, flash-algorithm, and
CMSIS-DAP v2 USB implementations include attributed adaptations from pyOCD. It
supports Linux, macOS, and Windows.

## Status

| Feature                                  | Status     | Notes |
| ---------------------------------------- | ---------- | ----- |
| Probe enumeration                        | ✅ Works   | Reads serial, product, manufacturer |
| DAP_Info / DAP_Connect / SWD             | ✅ Works   | |
| DP/AP register read/write                | ✅ Works   | |
| Memory read/write via AHB-AP             | ✅ Works   | 32-bit, auto-increment |
| Flash mass-erase via CTRL-AP             | ✅ Works   | |
| Flash programming via flash algorithm    | ✅ Works   | ~14 kB/s with `--no-verify` on XIAO nRF54LM20A |
| Flash verify (read-back)                  | ✅ Works   | Word-by-word verification, slower but robust |
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

# Program flash with chip erase, reset, and readback verification
nrf_ocd -t nrf54l15 -u 761FDE87 -e chip -R load firmware.hex

# Fast upload path used by the Arduino core
nrf_ocd -t nrf54l15 -u 761FDE87 -e chip -R --no-verify load firmware.hex

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

### External probes

Any CMSIS-DAP probe works, not only the XIAO's onboard one. Probes are
picked up by a known vendor ID or by "CMSIS-DAP" in their USB product
string, as the CMSIS-DAP spec requires.

Tested: **Flipper Zero** with the [DAP Link](https://lab.flipper.net/apps/dap_link)
app, on macOS (HID) with an nRF54L15. Wire SWCLK to GPIO **2** (A7) and
SWDIO to GPIO **3** (A6), plus 3V3 (9) and GND (11). Pins 10 and 12 are
labelled SWC/SIO on the case, but that is the Flipper's own debug port;
the app does not use them.

HID is slow: about 3 kB/s programming, and readback verify of a 600 KB
image takes around 25 minutes. Use `--no-verify` and spot-check with
`read` instead.

```sh
nrf_ocd -t nrf54l15 -u <serial> -e chip --no-verify load firmware.hex
```

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
arduino-cli core update-index && arduino-cli core install nrf54l15clean:nrf54l15clean@1.0.0
nrf_ocd -p /dev/ttyACM0 -t nrf54lm20a -e chip -R --no-verify load sketch.hex
```

## Known limitations

- **Speed**: The fast Arduino upload path uses `--no-verify` and was measured
  at about 14 kB/s on XIAO nRF54LM20A with a 104 KB image. Full readback
  verification is still supported with `--verify`, but remains slower because
  the safe verifier uses conservative word reads.
- **DAP_TransferBlock on HID**: The SAMD11 USB bridge truncates HID
  responses to 63 bytes. The bulk backend (USE_LIBUSB=1) avoids this
  issue. When using HID, the code tolerates truncated responses.

## License

Project-owned source is available under the [MIT License](LICENSE). The target,
flash-algorithm, and libusb transport files identified in
[Third-Party Notices](THIRD_PARTY_NOTICES.md) retain pyOCD material under
Apache-2.0. Component-specific terms take precedence over the project license.

## Release Compliance

Linux x86-64 release binaries use a statically linked libusb `1.0.27` and are
therefore not MIT-only artifacts. Releases that contain that binary must also
provide the exact libusb corresponding source, LGPL-2.1 text, relink
instructions, application source, and checksums. macOS, Windows, and Linux ARM
builds use native HID/WinUSB unless their release notes state otherwise.

See the release assets and [Third-Party Notices](THIRD_PARTY_NOTICES.md) before
redistributing a binary.

Current Linux x86-64 releases include version-matched source and exact libusb
source assets. To rebuild with the supplied or modified library, follow
[the relink procedure](compliance/RELINK_LINUX_X86_64.md).

Maintainers reconstructing the historical `v0.3.3` compliance bundle can use:

```sh
./scripts/create_v0.3.3_compliance_bundle.sh
```

## Support The Project

If nRF OCD saves you development time, please consider supporting its ongoing
maintenance, hardware testing, and release work.

<a href="https://buymeacoffee.com/lolren"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50"></a>
