# Source Provenance

The `v0.3.3` release executable source baseline is Git commit
`9e2ce3a1ba480c43cbb5f6b43a3d0acc47d83c92`. The corrective compliance
archive is generated from that tag by
`scripts/create_v0.3.3_compliance_bundle.sh`; post-tag executable changes on
`main` are not copied into the archive.

The compliance source archive distributed beside the existing `v0.3.3` Linux
x86-64 binaries applies redistribution-only metadata to that baseline:

- project and component license texts
- retained pyOCD copyright and Apache-2.0 notices prepended to the four
  pyOCD-derived C files
- third-party and LGPL distribution guidance
- removal of the unrelated stale `build_win` output
- the exact upstream libusb 1.0.27 archive with SHA-256
  `ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`
- a tested Linux x86-64 relink procedure and published-asset checksums

Those changes do not alter executable C statements. They make the corresponding
source self-describing without moving or rewriting the published `v0.3.3` tag.
