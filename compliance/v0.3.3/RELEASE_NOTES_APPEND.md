## Source and license materials

The Linux x86-64 assets (`nrf_ocd`, `nrf_ocd-linux-amd64`, and
`nrf_ocd-linux-x64`) statically include libusb 1.0.27. The
`open-nrf-ocd-v0.3.3-compliance.tar.gz` asset provides the complete nRF OCD
source at commit `9e2ce3a1ba480c43cbb5f6b43a3d0acc47d83c92`, the exact libusb 1.0.27 source
archive, license and attribution texts, published-binary checksums, and tested
instructions for rebuilding and relinking with a modified libusb.

Verify the bundle with `open-nrf-ocd-v0.3.3-compliance.tar.gz.sha256` before
extracting it. This is a source-compliance addition only; the `v0.3.3` tag and
the previously published executable assets are unchanged.
