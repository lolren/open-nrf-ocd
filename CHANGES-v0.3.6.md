# nrf_ocd v0.3.6 changes

## Reliable post-reset teardown

- Replace the 0.3.5 transport-only close with one short, best-effort
  `DAP_Disconnect` transaction. A live probe receives the CMSIS-DAP port-off
  command; a stale or re-enumerated USB handle is closed without retries or a
  misleading upload error after programming has already completed.
- Support both raw bulk replies and HID replies with or without a report-ID
  byte during the quiet teardown.
- Correct ordinary HID `DAP_Disconnect` response parsing.

## Bounded transfer recovery

- Remove recursive `DP_ABORT` recovery after an SWD `NO_ACK`. The original
  transfer is now retried exactly once after a line reset; a second `NO_ACK`
  returns an error instead of recursively consuming the process stack.
- Normalize bulk and HID transfer responses after every `WAIT` and `NO_ACK`
  retry so the ACK and read data are decoded from the correct offsets.

## Validation

- Add unit coverage for bulk and HID teardown, single-attempt failure paths,
  ordinary disconnect parsing, and target close ownership.
- Hardware validation programmed and booted 129,904-byte and 133,980-byte
  images on XIAO nRF54L15 and XIAO nRF54LM20A targets respectively. Both
  probes and CDC ports remained enumerated after reset.
