# nrf_ocd v0.3.3 changes

## Recovery

- Corrected CMSIS-DAP `DAP_SWJ_Pins` packet encoding so nRESET pulses include
  the required pin-select byte and 32-bit wait field.
- Added nRESET pulses on SWD reconnect attempts to wake targets that are in
  sleep/system-off states before flashing.
- Added a longer retry window after CTRL-AP APPROTECT mass erase before
  halting/programming the target again.

## USB transport

- Retry bulk CMSIS-DAP transfers after clearing endpoint halt state and
  draining stale IN packets.
- Close the underlying USB handle when a target is closed, allowing retry
  attempts to reopen a clean CMSIS-DAP session.
- Filter libusb enumeration to actual CMSIS-DAP v2 interfaces and fall back to
  Linux sysfs descriptors for UID/product strings when USB string reads fail.
