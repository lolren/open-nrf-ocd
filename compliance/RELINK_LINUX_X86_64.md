# Relinking the Linux x86-64 Binary

Linux x86-64 release binaries statically include libusb 1.0.27. Each release
provides the complete version-matched nrf_ocd source archive and the unmodified
`libusb-1.0.27.tar.bz2` corresponding source asset.

Place the libusb archive in the extracted nrf_ocd source root, or point to it
explicitly, then run:

```sh
LIBUSB_ARCHIVE="$PWD/libusb-1.0.27.tar.bz2" \
  ./compliance/relink_linux_x86_64.sh
```

The result is written to `relink-output/nrf_ocd-linux-amd64`. Byte identity is
not promised because Zig and system build-tool versions may differ; the output
is a working executable relinked from the supplied library source.

To exercise the LGPL right to modify and relink libusb, extract and edit it,
then pass that source directory as the first argument. Modified source trees
are intentionally not checked against the upstream archive hash:

```sh
tar -xjf libusb-1.0.27.tar.bz2
./compliance/relink_linux_x86_64.sh "$PWD/libusb-1.0.27"
```

Prerequisites are Bash, Zig, make, tar, awk, and sha256sum. The libusb source
tree must contain the release-generated `configure` script.
