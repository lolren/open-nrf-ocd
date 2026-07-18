# Third-Party Notices

The root [MIT License](LICENSE) applies to project-owned source unless a file
states different terms. Imported and adapted material retains the licenses and
copyright notices below.

## pyOCD

The following files contain material adapted from pyOCD `0.44.1` and are
distributed under [Apache-2.0](LICENSES/Apache-2.0.txt):

- `src/flash_algo_nrf54l.c`
- `src/target_nrf54l.c`
- `src/target_nrf54lm20a.c`
- `src/hid_libusb.c`

Their file headers retain the applicable Arm, Nordic Semiconductor, Monadnock
Systems, StarSphere, mentha, Chris Reed, and Lars Häring notices and describe
the nRF OCD adaptations. pyOCD is maintained at
<https://github.com/pyocd/pyOCD>.

## libusb

Linux x86-64 release binaries built with `build_for_core.sh` statically link
libusb `1.0.27` under LGPL-2.1-or-later. Those binary releases are therefore not
MIT-only artifacts. Current releases provide the exact libusb source, complete
[LGPL-2.1 text](LICENSES/LGPL-2.1-or-later.txt), complete version-matched nRF
OCD application source, and a
[relink procedure](compliance/RELINK_LINUX_X86_64.md) that accepts modified
libusb source. The historical `v0.3.3` compliance bundle remains available to
reconstruct the older 0.3.3 assets.

The macOS, Windows, and Linux ARM builds use their native HID/WinUSB transport
and do not include libusb unless a release explicitly says otherwise.
