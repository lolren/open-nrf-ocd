# nrf_ocd v0.3.8 changes

## Onboard probe target guard

- Refuse to open an nRF54L15 target through the XIAO nRF54LM20A onboard
  CMSIS-DAP probe, or an nRF54LM20A target through the XIAO nRF54L15 probe.
- Perform the check from the probe USB VID/PID before sending any CMSIS-DAP
  command, including when probe selection falls back to the only connected
  device because its serial descriptor is unreadable.
- Leave external and unrecognised CMSIS-DAP probes unrestricted because their
  USB identity does not determine the attached target.

## Validation

- Add unit coverage for both matching onboard probe/target pairs, both crossed
  pairs, and external or unrecognised probes.
- Retain the zero-command post-reset teardown and transport recovery coverage
  from 0.3.7.
