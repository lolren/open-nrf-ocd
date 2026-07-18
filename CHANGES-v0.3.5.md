# nrf_ocd v0.3.5 changes

## Post-reset USB teardown

- Close the USB transport directly after a successful post-flash or standalone
  target reset instead of sending a late CMSIS-DAP `DAP_Disconnect` command.
  On probes that re-enumerate at reset, that command targeted an invalid bulk
  handle and could report an I/O error after programming had already reached
  100%.
- Keep graceful `DAP_Disconnect` teardown for failed resets, `--no-reset`
  uploads, non-flash commands, and every error path.
- Add mock-backed regression coverage proving both teardown modes still close
  the saved USB handle and clear target state.

## Validation

- Programmed a 133,980-byte image on XIAO nRF54LM20A and a 129,904-byte image
  on XIAO nRF54L15 with chip erase and post-flash reset enabled.
- Both onboard CMSIS-DAP probes and serial ports remained enumerated after the
  upload, with no late command `0x03` transport error.
