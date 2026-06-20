# nrf_ocd v0.3.1 changes

Released: 2026-06-20

## Windows transport and Arduino IDE fixes

- Added native CMSIS-DAP v2 WinUSB enumeration and bulk endpoint support.
- Fixed corrupted Windows device paths and reliable probe opening.
- Added COM-port-to-CMSIS-DAP serial mapping for Arduino IDE `-p COMx` uploads.
- Correctly identifies Seeed PID `0x0068` as `nrf54lm20a`.
- Replaced the GNU `getopt` dependency with a portable CLI parser.
- Fixed MSVC compatibility for logging annotations and platform includes.
- Added required Windows link libraries, including WinUSB, CfgMgr32, and Advapi32.

## Safety and reliability

- Locked targets are no longer mass-erased by diagnostic commands.
- Mass erase now requires `--auto-unlock`, an explicit chip erase request, or the `erase` command.
- Avoids a duplicate chip erase when unlock already erased the device.
- Rejects unsupported sector erase instead of silently performing a chip erase.
- Fixed an uninitialized core-status value and several declaration/header issues.

## Validation

- Windows x64 production build completed with MSVC and static runtime.
- Windows WinUSB enumeration and Arduino-style COM-port selection passed with three connected probes.
- Linux unit tests passed for HEX, ELF, and target lookup.
- Linux amd64 release built with libusb and Zig targeting glibc 2.17.

## Release binaries

| Asset | SHA-256 |
|---|---|
| `nrf_ocd-linux-amd64` | `1A6EE7714698B67FBCF93E8602D23D9CDAB976119986B60155FA3A6EE11BCD80` |
| `nrf_ocd-windows-amd64.exe` | `9A456DCDC80ED57DA410A49F9E179BE1BEADD39E7A22909A045C3ED1142246F7` |

The repository uses `main` as its sole development branch. The obsolete divergent `master` branch was removed during this release.
