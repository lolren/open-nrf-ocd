# nrf_ocd — Native C CMSIS-DAP Flash Programmer for nRF54

**A zero-Python, portable, single-binary replacement for pyOCD**, purpose-built for
Nordic nRF54L15 and nRF54LM20A ARM Cortex-M33 microcontrollers.

`nrf_ocd` implements the full CMSIS-DAP v1/v2 debug probe protocol and CoreSight
SWD debug interface in pure C — no Python, no pip, no virtual environments, no 50+
transitive dependencies. Everything in one binary.

```
$ nrf_ocd -l
#   Probe/Board                                          Unique ID   Target
--------------------------------------------------------------------------------
 0   Seeed Studio XIAO nRF54LM20A CMSIS-DAP                 3377B9D6
 1   Seeed Studio XIAO nrf54 CMSIS-DAP                      761FDE87
 2   Debugprobe on Pico (CMSIS-DAP)                         E66118C4E3249A25

$ nrf_ocd -u 761FDE87 -e -f blink.ino.hex
INFO:  SWD clock: 4000000 Hz
INFO:  SWD connected successfully on attempt 1
INFO:  Connected to device, DP IDCODE = 0x6BA02477
INFO:  Programming 11972 bytes from 1 segment(s)...
INFO:  Done: 11972 bytes programmed, 3 sectors erased
```

## Features

| Capability | `nrf_ocd` | pyOCD |
|---|---|---|
| List connected probes | ✓ `-l` | ✓ |
| Device info (part, flash size, security) | ✓ `-i` | ✓ |
| Mass erase | ✓ `-e` | ✓ |
| Sector erase | ✓ `-e -s ADDR` | ✓ |
| Flash .hex files | ✓ `-f` | ✓ |
| Erase + flash | ✓ `-e -f` | ✓ |
| Memory read (hex dump) | ✓ `-r ADDR LEN` | ✓ |
| Memory write (32-bit word) | ✓ `-w ADDR VALUE` | ✓ |
| Target reset | ✓ `-R` | ✓ |
| Auto-unlock secure devices | ✓ `--auto-unlock` | ✓ |
| SWD clock control | ✓ `-c` (default 4MHz) | ✓ |
| Connect mode (halt/attach) | ✓ `--connect` | ✓ |
| CMSIS-DAP v1 (HID) | ✓ | ✓ |
| CMSIS-DAP v2 (bulk / WinUSB) | ✓ | ✓ |
| **External dependencies** | **0 on Windows** | **~50 Python packages** |
| **Binary size** | **~100KB** | N/A |
| Flash 115KB | ~5.8s (4MHz) | ~4.8s |

## Supported Hardware

### Target MCUs
- **nRF54L15** (1.5MB flash, 256KB RAM)
- **nRF54LM20A** (2.0MB flash, 512KB RAM)

### Debug Probes (any CMSIS-DAP v1 or v2)
| Probe | VID:PID | Transport | Tested |
|---|---|---|---|
| XIAO nRF54L15 built-in | `2886:0066` | libusb v2 bulk | ✓ |
| XIAO nRF54LM20A built-in | `2886:0068` | libusb v2 bulk | ✓ |
| Raspberry Pi Debugprobe on Pico | `2e8a:000c` | libusb v2 bulk | ✓ |
| DAPLink (all variants) | various | HID v1 or v2 | supported |
| J-Link (CMSIS-DAP mode) | `1366:0101/0105` | v1/v2 | supported |
| ST-Link v2/v3 | `0483:374b/3748` | HID v1 | supported |

### Boards Tested
- Seeed Studio XIAO nRF54L15
- Seeed Studio XIAO nRF54LM20A
- HolyIoT 25008 nRF54L15 Module (via picotool debug probe)

---

## Installation

### Download Pre-built Binaries

Go to the [Releases](../../releases) page and download the binary for your platform:

- **Linux**: `nrf_ocd-linux-amd64` — requires `apt install libhidapi-libusb0 libusb-1.0-0`
- **macOS**: `nrf_ocd-darwin-amd64` — requires `brew install hidapi libusb`
- **Windows**: `nrf_ocd-windows-amd64.exe` — **zero dependencies**, just run it

```bash
# Linux/macOS
chmod +x nrf_ocd
sudo ./nrf_ocd -l

# Windows
nrf_ocd.exe -l
```

### Build from Source

#### Linux (native)
```bash
sudo apt install build-essential cmake pkg-config libhidapi-libusb-dev libusb-1.0-0-dev

git clone https://github.com/your-org/nrf_ocd.git
cd nrf_ocd
cmake -B build -S .
cmake --build build
sudo cp build/nrf_ocd /usr/local/bin/
```

#### macOS (native)
```bash
brew install cmake hidapi libusb pkg-config

git clone https://github.com/your-org/nrf_ocd.git
cd nrf_ocd
cmake -B build -S .
cmake --build build
sudo cp build/nrf_ocd /usr/local/bin/
```

#### Windows (native MSYS2/MinGW)
```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc

git clone https://github.com/your-org/nrf_ocd.git
cd nrf_ocd
cmake -B build -S .
cmake --build build
# nrf_ocd.exe is now in build/
```

#### Windows (cross-compile from Linux)
```bash
sudo apt install gcc-mingw-w64-x86-64 cmake

cd nrf_ocd
cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake
cmake --build build_win
# Output: build_win/nrf_ocd.exe (fully static, zero dependencies)
```

---

## Usage

```
nrf_ocd - Native CMSIS-DAP flash programmer for nRF54 (pyOCD replacement)

Commands:
  nrf_ocd -l                              List connected CMSIS-DAP probes
  nrf_ocd -u <serial> -i                  Show target device info
  nrf_ocd -u <serial> -e                  Mass erase all flash
  nrf_ocd -u <serial> -e -s ADDR          Sector erase at address
  nrf_ocd -u <serial> -f <file.hex>       Program .hex file
  nrf_ocd -u <serial> -f <hex> -e         Erase then program
  nrf_ocd -u <serial> -r ADDR LEN         Read memory (hex dump)
  nrf_ocd -u <serial> -w ADDR VALUE       Write 32-bit word
  nrf_ocd -u <serial> -R                  Reset target

Options:
  -t <target>   Target: nrf54l15 (default), nrf54lm20a
  -c <freq>     SWD clock in Hz (default: 4000000, max: 8000000)
  --auto-unlock Auto mass-erase locked devices (CTRL-AP erase)
  --connect MODE Connect mode: halt (default), attach
  --no-reset    Don't reset after programming
  -v            Verbose debug output
  -q            Quiet mode
  -h            Show this help
```

### Examples

```bash
# List all connected CMSIS-DAP probes
nrf_ocd -l

# Show device info
nrf_ocd -u 761FDE87 -i

# Erase and flash
nrf_ocd -u 761FDE87 -e -f firmware.ino.hex

# Flash only (no erase — for pre-erased chips)
nrf_ocd -u 761FDE87 -f firmware.ino.hex

# Read 64 bytes from flash address 0x00 (vector table)
nrf_ocd -u 761FDE87 -r 0x00 64

# Write a 32-bit word to RAM
nrf_ocd -u 761FDE87 -w 0x20000000 0xDEADBEEF

# Reset target after operations
nrf_ocd -u 761FDE87 -R

# Flash a locked device (auto mass-erase to unlock)
nrf_ocd -u 761FDE87 --auto-unlock -e -f firmware.ino.hex

# Use lower SWD clock for long cables or noisy environments
nrf_ocd -u E66118C4E3249A25 -c 1000000 -i

# Target override for nRF54LM20A
nrf_ocd -u 3377B9D6 -t nrf54lm20a -e -f blink.ino.hex
```

---

## Architecture

`nrf_ocd` is built as a clean 7-layer stack, each layer a single C file:

```
┌──────────────────────────────────────────────────┐
│  src/nrf_ocd.c      CLI + Programmer             │  Parse args, orchestrate
├──────────────────────────────────────────────────┤
│  src/flash_algo.c   Flash Algorithm              │  nRF54L15/LM20A Cortex-M33
│  src/intelhex.c     Intel HEX Parser             │  Record types 00-04, checksum
├──────────────────────────────────────────────────┤
│  src/coresight.c    CoreSight DP/AP + MEM-AP     │  SWD connect, DP/AP regs, RAM
├──────────────────────────────────────────────────┤
│  src/cmsis_dap.c    CMSIS-DAP v1/v2 Protocol    │  DAP_Transfer retry, caching
├──────────────────────────────────────────────────┤
│  src/usb_backend.c  USB Transport (Linux/macOS)  │  hidapi + libusb
│  src/usb_backend_win.c  USB Transport (Windows)  │  setupapi + hid + winusb
├──────────────────────────────────────────────────┤
│  src/log.c          Logging + Error Strings      │  Leveled, thread-safe
├──────────────────────────────────────────────────┤
│  include/nrf_ocd.h  Public API                   │  Types, constants, prototypes
│  include/platform.h Cross-platform compat        │  usleep, Sleep
└──────────────────────────────────────────────────┘
```

### Layer: USB Transport (`usb_backend.c` / `usb_backend_win.c`)

Abstracts HID and bulk USB endpoints behind a uniform `nrf_probe_t` interface:

- **Linux/macOS** (`usb_backend.c`): Uses `hidapi` for HID enumeration and I/O, `libusb-1.0`
  for CMSIS-DAP v2 bulk endpoints. Handles v1 HID report ID stripping, v2 bulk packet
  padding, and stale report flushing.

- **Windows** (`usb_backend_win.c`): **Zero external dependencies**. Uses native Windows
  APIs: `SetupDiGetClassDevs` for HID enumeration, `CreateFile`/`ReadFile`/`WriteFile` for
  HID I/O, and `WinUsb` for v2 bulk. No hidapi, no libusb — just kernel32, setupapi,
  hid, and winusb DLLs (shipped with every Windows since Vista).

### Layer: CMSIS-DAP Protocol (`cmsis_dap.c`)

Implements all CMSIS-DAP v1/v2 commands needed for flash programming:

| Command | ID | Description |
|---------|----|-------------|
| `DAP_Info` | 0x00 | Query probe capabilities, version, packet size |
| `DAP_Connect` | 0x02 | Enter SWD or JTAG mode |
| `DAP_Disconnect` | 0x03 | Exit debug mode |
| `DAP_TransferConfigure` | 0x04 | Set idle cycles, WAIT retry count |
| `DAP_Transfer` | 0x05 | Single DP/AP register read/write |
| `DAP_TransferBlock` | 0x06 | Bulk DP/AP register read/write |
| `DAP_WriteAbort` | 0x08 | Abort stuck transfer |
| `DAP_ResetTarget` | 0x0A | Hardware reset via nRESET pin |
| `DAP_SWJ_Pins` | 0x10 | Control SWCLK/SWDIO/nRESET pins |
| `DAP_SWJ_Clock` | 0x11 | Set SWD/JTAG clock frequency |
| `DAP_SWD_Sequence` | 0x1D | Send SWD bit sequences (line reset) |

**Retry Logic**: Transfers implement exponential backoff for `ACK_WAIT` responses,
sticky-error clearing on `ACK_FAULT`, and full SWD line reset on protocol errors.
This matches pyOCD's error recovery behavior.

**DP SELECT Caching**: The DP_SELECT register value is cached and only rewritten
when the AP bank selection changes, eliminating redundant USB transfers.

### Layer: CoreSight DP/AP + MEM-AP (`coresight.c`)

Implements the ARM CoreSight Debug Port and Access Port protocol:

- **SWD Connect**: 5-attempt retry with `DAP_SWD_Sequence` line reset (>50 SWCLK
  cycles with SWDIO high, then 8 idle, then DP IDCODE read). Matches pyOCD's
  SWJSequenceSender exactly.

- **Power-up Sequence**: Writes `DP_SELECT = 0`, clears sticky errors, requests
  debug+system power with `0x50000000` (matching pyOCD's DebugPortStart hook),
  and waits for both `CDBGPWRUPACK` and `CSYSPWRUPACK`.

- **MEM-AP CSW Management**: Reads the hardware default CSW from the AHB-AP
  during initialization and preserves it (SIZE, SADDRINC, DEVICEEN, HPROT bits).
  CSW is written once during init and never touched again — avoiding CSW
  corruption that causes transfer faults.

- **CTRL-AP Mass Erase**: For locked devices (`--auto-unlock`), performs the
  Nordic-specific CTRL-AP ERASEALL sequence with proper status polling and
  reset handshake.

### Layer: Flash Algorithm (`flash_algo.c`)

Executes Nordic's position-independent Thumb-2 flash algorithms on the target
Cortex-M33 core:

1. **Halt core** via DHCSR write (`C_DEBUGEN | C_HALT`)
2. **Load algorithm** to RAM at `0x20000000` via DAP_TransferBlock
3. **Set up registers** (R0-R3 args, R9=static_base, R13=stack_top,
   R14=return to BKPT, R15=entry point)
4. **Resume core** — algorithm runs, returns to BKPT, core halts
5. **Read R0** — check return value for errors

| Function | Address | Operation | Timeout |
|----------|---------|-----------|---------|
| `Init` | `0x20000015` | Configure NVMC | 15s |
| `UnInit` | `0x20000019` | Restore NVMC | 15s |
| `EraseAll` | `0x2000001D` | Mass erase | 120s |
| `EraseSector` | `0x20000041` | 4KB sector erase | 60s |
| `ProgramPage` | `0x20000065` | Write page (up to 1024B) | 60s |

Both `nrf54l15` and `nrf54lm20a` targets use the same flash algorithm (verified
against pyOCD's `target_nRF54L15.py` and `target_nRF54LM20A.py`).

### Layer: Intel HEX Parser (`intelhex.c`)

Parses Intel HEX format files with contiguous segment merging for efficient
flash programming. Handles record types 00 (data), 01 (EOF), 02 (extended
segment address), and 04 (extended linear address).

Uses fixed-length hex byte parsing (not `strtol` which would overflow on
all-hex strings) with proper checksum verification.

### Layer: Logging (`log.c`)

Lightweight leveled logging (ERROR/WARN/INFO/DEBUG) writing to stderr with
fflush — safe for use in signal handlers and crash paths. Error code-to-string
conversion for all 16 error codes.

---

## Technical Details

### SWD Connect Sequence

The connect sequence exactly mirrors pyOCD's proven flow:

```
1. DAP_Connect(port=SWD)          Enter SWD mode on probe
2. DAP_SWD_Configure(turnaround=1) Set 1-cycle turnaround
3. DAP_TransferConfigure(         Configure transfer engine
     idle=2, wait_retry=150)
4. DAP_SWD_Sequence(              SWD line reset:
     51 cycles SWDIO high,           >50 clocks with data high
     8 cycles SWDIO low)             at least 2 idle cycles
5. nrf_dp_read(DP_IDCODE)         Exit reset, verify target
6. nrf_dp_write(DP_ABORT,         Clear sticky error flags
     ORUNERRCLR|WDERRCLR|
     STKERRCLR|STKCMPCLR)
7. nrf_dp_write(DP_SELECT, 0)     Bank 0
8. nrf_dp_write(DP_CTRL_STAT,     Request power
     0x50000000)                     CSYSPWRUPREQ|CDBGPWRUPREQ
9. Wait for CSYSPWRUPACK|CDBGPWRUPACK
10. nrf_dp_write(DP_ABORT, ...)   Final sticky error clear
```

On failure, retry up to 5 times with exponential backoff (50ms × 2^attempt).

### Transfer Retry Logic

```
DAP_Transfer / DAP_TransferBlock
         │
    ACK_OK? ──yes──> return data
         │
    ACK_WAIT? ──yes──> retry (probe handles WAIT internally
         │             via TransferConfigure, but SW-level
         │             retry adds robustness)
    ACK_FAULT? ──yes──> dap_clear_sticky(DP_ABORT)
         │             retry up to 3 times
    protocol error? ──yes──> dap_swd_line_reset()
         │                  retry once
    return error
```

### Flash Programming Flow

```
For each contiguous HEX segment:
│
├─ For each 4KB sector with data:
│   ├─ nrf_flash_erase_sector(addr)
│   │   ├─ Halt core (DHCSR)
│   │   ├─ Load flash algo to RAM (DAP_TransferBlock, 32-bit words)
│   │   ├─ Call EraseSector(R0=addr)
│   │   └─ Wait for halt, check R0
│   │
│   └─ For each page within sector:
│       ├─ nrf_flash_program_page(addr, data, len)
│       │   ├─ Convert bytes to 32-bit words
│       │   ├─ Write words to page buffer via DAP_TransferBlock
│       │   ├─ Call ProgramPage(R0=addr, R1=len, R2=buffer)
│       │   └─ Wait for halt, check R0
│       └─ Progress: bytes/total (percent)
```

### Secure Device Handling (`--auto-unlock`)

When APPROTECT is enabled on the nRF54:

1. Read CTRL-AP IDR at AP#2 → expect `0x32880000`
2. Write `1` to CTRL-AP ERASEALL
3. Poll CTRL-AP ERASEALLSTATUS:
   - `BUSY (0x2)` → mass erase in progress
   - `READYTORESET (0x1)` → erase complete
   - `ERROR (0x3)` → abort
4. Write CTRL-AP RESET:
   - `2` → trigger reset
   - `0` → release reset
5. Wait 200ms for device to reboot
6. Reconnect SWD — device is now unlocked

---

## Performance

Flash speed comparison with pyOCD (XIAO nRF54L15, 115KB hex file):

| Tool | Clock | Time | Speed |
|------|-------|------|-------|
| pyOCD | 1 MHz | ~4.8s | 45.8 kB/s |
| **nrf_ocd** | 4 MHz | ~5.8s | ~20 kB/s |
| nrf_ocd | 1 MHz | ~11s | ~10 kB/s |

The speed difference is primarily due to:
1. pyOCD uses 32-word DAP_TransferBlock chunks; nrf_ocd uses 14-word chunks
   (limited by 64-byte CMSIS-DAP v2 packet size on XIAO probes)
2. pyOCD skips erasing already-erased sectors (CRC trust); nrf_ocd always erases
3. pyOCD benefits from Python's async I/O pipelining

These optimizations are planned for future releases.

---

## Memory Read Comparison (vs pyOCD)

All three tested boards show exact memory value matches between nrf_ocd and pyOCD:

| Board | Address | nrf_ocd | pyOCD | Match |
|-------|---------|---------|-------|-------|
| XIAO nRF54L15 | `0x00000000` | `20026000` | `20026000` | ✓ |
| XIAO nRF54LM20A | `0x00000000` | `20070000` | `20070000` | ✓ |
| HolyIoT 25008 + picotool | `0x00000000` | `20026000` | `20026000` | ✓ |

---

## Project Structure

```
open_nrf_ocd/
├── CMakeLists.txt              Build system (Linux/macOS/Windows)
├── CMakeLists_win.txt          Windows-specific build (reference)
├── mingw64.cmake               MinGW cross-compilation toolchain
├── README.md                   This file
├── .gitignore
├── include/
│   ├── nrf_ocd.h               Public API: types, constants, prototypes
│   └── platform.h              usleep() cross-platform shim
└── src/
    ├── nrf_ocd.c               CLI entry point + programmer orchestration
    ├── usb_backend.c           USB transport (Linux/macOS: hidapi + libusb)
    ├── usb_backend_win.c       USB transport (Windows: native API)
    ├── cmsis_dap.c             CMSIS-DAP v1/v2 protocol
    ├── coresight.c             CoreSight SWD, DP/AP, MEM-AP, CTRL-AP
    ├── flash_algo.c            nRF54L15 / nRF54LM20A flash algorithms
    ├── intelhex.c              Intel HEX file parser
    └── log.c                   Logging and error strings
```

---

## License

MIT License — see LICENSE file.

## Credits

Built as a portable replacement for [pyOCD](https://github.com/pyocd/pyOCD),
the Python-based Cortex-M debugger. Flash algorithms and CTRL-AP sequences
derived from pyOCD's Nordic target support.

---

## Building Release Binaries

```bash
# Clean build for all platforms
cd open_nrf_ocd

# Linux
rm -rf build && cmake -B build -S . && cmake --build build
cp build/nrf_ocd nrf_ocd-linux-amd64

# Windows (cross-compile)
rm -rf build_win && cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake
cmake --build build_win
cp build_win/nrf_ocd.exe nrf_ocd-windows-amd64.exe

# macOS (must be built on macOS — cross-compilation not supported)
# brew install cmake hidapi libusb pkg-config
# rm -rf build && cmake -B build -S . && cmake --build build
# cp build/nrf_ocd nrf_ocd-darwin-amd64
```
