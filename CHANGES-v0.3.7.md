# nrf_ocd v0.3.7 changes

## SAMD11-safe post-reset teardown

- Do not send `DAP_Disconnect` or any other CMSIS-DAP command after a
  successful target reset. The XIAO onboard SAMD11 probe may re-enumerate or
  stop accepting commands at this boundary, so the uploader now closes only
  the saved USB handle.
- Keep normal `DAP_Disconnect` teardown for live sessions that did not reset
  the target, including failed resets, `--no-reset` uploads, register access,
  and error paths.
- Preserve the bounded `WAIT` and `NO_ACK` recovery and response parsing fixes
  introduced in 0.3.6.

## Validation

- Add mock-backed bulk and HID tests proving post-reset teardown performs zero
  USB reads, writes, or flushes while still clearing protocol state.
- Retain ordinary bulk and HID disconnect tests for live-session teardown.
