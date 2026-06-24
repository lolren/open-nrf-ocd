# nrf_ocd v0.3.2 changes

## Upload speed

- Removed the fixed 1 ms host-side delay after every flash-program chunk.
- Kept the reliable 1024-byte flash-program chunk size. Larger 2048/4096-byte
  chunks were tested on XIAO nRF54LM20A and rejected because readback verify
  caught corruption at address 0.
- Arduino core uploads should call `nrf_ocd --no-verify` for the normal fast
  path. Full readback verification remains available by omitting
  `--no-verify` or by passing `--verify`.

Measured on XIAO nRF54LM20A with a 104264-byte image:

| Mode | Result |
| --- | --- |
| v0.3.1 default readback verify | 36.54 s wall time |
| v0.3.2 `--no-verify` | 6.87 s wall time |
| v0.3.2 default readback verify | 36.79 s wall time |

## Robustness

- Fixed `DAP_TransferBlock` packet count setup so the requested transfer count
  matches the clamped packet-sized chunk for both reads and writes.
- Added `build_release_binaries.sh` to build the exact Linux and Windows assets
  used by the Arduino core fallback downloader.
