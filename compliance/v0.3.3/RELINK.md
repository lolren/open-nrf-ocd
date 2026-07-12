# Relinking the v0.3.3 Linux x86-64 Binary

The published `nrf_ocd`, `nrf_ocd-linux-amd64`, and `nrf_ocd-linux-x64`
assets statically include libusb 1.0.27. This compliance bundle contains the
complete nRF OCD application source and the exact upstream libusb archive used
by the release build path.

The included script rebuilds nRF OCD against the bundled libusb source:

```sh
./compliance/v0.3.3/relink_linux_x86_64.sh
```

The result is written to `relink-output/nrf_ocd-linux-amd64`. A byte-identical
result is not promised because Zig and system build tools may differ from the
original release environment; the purpose of this procedure is to produce a
working executable relinked with the supplied or modified library.

To relink with a modified libusb, extract `third_party/libusb-1.0.27.tar.bz2`,
edit that source tree, and pass it to the script:

```sh
tar -xjf third_party/libusb-1.0.27.tar.bz2
./compliance/v0.3.3/relink_linux_x86_64.sh "$PWD/libusb-1.0.27"
```

Prerequisites are Bash, Zig, make, tar, and sha256sum. A supplied libusb source
tree must contain the release-generated `configure` script.
