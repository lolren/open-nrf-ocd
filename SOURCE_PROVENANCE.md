# Source Provenance

The complete executable source for `nrf_ocd` 0.3.6 is the repository tree at
the annotated Git tag `v0.3.6`:

<https://github.com/lolren/open-nrf-ocd/tree/v0.3.6>

The release source asset is generated directly from that tag with no source
substitution:

```bash
git archive --format=tar.gz --prefix=open-nrf-ocd-v0.3.6/ \
  -o open-nrf-ocd-v0.3.6-source.tar.gz v0.3.6
```

The repository contains the project and component license texts, retained
pyOCD attribution on the derived C sources, third-party notices, and the
[Linux x86-64 relink procedure](compliance/RELINK_LINUX_X86_64.md). That
procedure accepts both the exact library archive and a modified libusb source
tree. The corresponding unmodified libusb 1.0.27 source archive is distributed
beside the binary and source assets with SHA-256
`ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`.

For historical 0.3.3 compliance reconstruction, use
`scripts/create_v0.3.3_compliance_bundle.sh`; that script is intentionally
specific to the older `v0.3.3` tag and is not used to create 0.3.6 sources.
