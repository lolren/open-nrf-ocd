#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export LC_ALL=C

TAG="v0.3.3"
EXPECTED_COMMIT="9e2ce3a1ba480c43cbb5f6b43a3d0acc47d83c92"
LIBUSB_VERSION="1.0.27"
LIBUSB_SHA256="ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/nrf_ocd"
LIBUSB_ARCHIVE="${LIBUSB_ARCHIVE:-$CACHE_DIR/libusb-${LIBUSB_VERSION}.tar.bz2}"
DIST="$ROOT/release_dist"
ARCHIVE_ROOT="open-nrf-ocd-${TAG}-compliance"
OUTPUT="$DIST/${ARCHIVE_ROOT}.tar.gz"

test "$(git rev-parse "${TAG}^{commit}")" = "$EXPECTED_COMMIT"
for path in LICENSE LICENSES/Apache-2.0.txt LICENSES/LGPL-2.1-or-later.txt \
            SOURCE_PROVENANCE.md THIRD_PARTY_NOTICES.md \
            compliance/v0.3.3/PUBLISHED_ASSET_SHA256SUMS.txt \
            compliance/v0.3.3/RELEASE_NOTES_APPEND.md \
            compliance/v0.3.3/RELINK.md \
            compliance/v0.3.3/relink_linux_x86_64.sh; do
    test -f "$path"
done

mkdir -p "$CACHE_DIR" "$DIST"
if [ ! -f "$LIBUSB_ARCHIVE" ]; then
    curl --fail --location --retry 3 --output "$LIBUSB_ARCHIVE.part" "$LIBUSB_URL"
    mv "$LIBUSB_ARCHIVE.part" "$LIBUSB_ARCHIVE"
fi
printf '%s  %s\n' "$LIBUSB_SHA256" "$LIBUSB_ARCHIVE" | sha256sum --check --status

WORK="$(mktemp -d "${TMPDIR:-/tmp}/nrf_ocd-compliance.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/$ARCHIVE_ROOT"
mkdir -p "$TREE"
git archive "$TAG" | tar -xf - -C "$TREE"
rm -rf "$TREE/build_win"

cp LICENSE SOURCE_PROVENANCE.md THIRD_PARTY_NOTICES.md "$TREE/"
cp -a LICENSES "$TREE/"
mkdir -p "$TREE/compliance/v0.3.3" "$TREE/scripts" "$TREE/third_party"
cp compliance/v0.3.3/PUBLISHED_ASSET_SHA256SUMS.txt \
   compliance/v0.3.3/RELEASE_NOTES_APPEND.md \
   compliance/v0.3.3/RELINK.md \
   compliance/v0.3.3/relink_linux_x86_64.sh "$TREE/compliance/v0.3.3/"
cp scripts/create_v0.3.3_compliance_bundle.sh "$TREE/scripts/"
cp build_for_core.sh "$TREE/"
cp "$LIBUSB_ARCHIVE" "$TREE/third_party/libusb-${LIBUSB_VERSION}.tar.bz2"
chmod 0755 "$TREE/build_for_core.sh" \
           "$TREE/compliance/v0.3.3/relink_linux_x86_64.sh" \
           "$TREE/scripts/create_v0.3.3_compliance_bundle.sh"

prepend_notice() {
    local path="$1"
    local notice="$2"
    local original="$WORK/original-$(basename "$path")"
    cp "$path" "$original"
    printf '%s\n\n' "$notice" > "$path"
    cat "$original" >> "$path"
}

prepend_notice "$TREE/src/flash_algo_nrf54l.c" '/* Copyright (c) 2010-2023 Nordic Semiconductor ASA. All rights reserved.
 * Copyright (c) 2025 StarSphere. All rights reserved.
 * Copyright (c) 2026 Arm Limited
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted for nRF OCD in 2026 from pyOCD nRF54L flash-algorithm data and
 * execution behavior. See LICENSES/Apache-2.0.txt and THIRD_PARTY_NOTICES.md.
 */'
prepend_notice "$TREE/src/target_nrf54l.c" '/* Copyright (c) 2006-2013 Arm Limited
 * Copyright (c) 2010-2023 Nordic Semiconductor ASA. All rights reserved.
 * Copyright (c) 2019 Monadnock Systems Ltd.
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * Copyright (c) 2026 Arm Limited
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted for nRF OCD in 2026 from pyOCD nRF54L target behavior. See
 * LICENSES/Apache-2.0.txt and THIRD_PARTY_NOTICES.md.
 */'
prepend_notice "$TREE/src/target_nrf54lm20a.c" '/* Copyright (c) 2006-2013 Arm Limited
 * Copyright (c) 2019 Monadnock Systems Ltd.
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * Copyright (c) 2025 StarSphere. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted for nRF OCD in 2026 from pyOCD nRF54L and nRF54LM20A target
 * behavior. See LICENSES/Apache-2.0.txt and THIRD_PARTY_NOTICES.md.
 */'
prepend_notice "$TREE/src/hid_libusb.c" '/* Copyright (c) 2019-2021, 2025 Arm Limited
 * Copyright (c) 2021 mentha
 * Copyright (c) 2021-2023 Chris Reed
 * Copyright (c) 2025 Lars Häring
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted for nRF OCD in 2026 from pyOCD CMSIS-DAP v2 USB behavior. See
 * LICENSES/Apache-2.0.txt and THIRD_PARTY_NOTICES.md.
 */'

printf '%s\n' "$EXPECTED_COMMIT" > "$TREE/SOURCE_COMMIT"
(
    cd "$TREE"
    find . -type f ! -name SHA256SUMS -print0 \
        | sort -z \
        | xargs -0 sha256sum > SHA256SUMS
    sha256sum --check SHA256SUMS
)

SOURCE_DATE_EPOCH="$(git show -s --format=%ct "${TAG}^{commit}")"
rm -f "$OUTPUT" "$OUTPUT.sha256"
tar --sort=name --format=posix --pax-option=delete=atime,delete=ctime \
    --mtime="@$SOURCE_DATE_EPOCH" \
    --owner=0 --group=0 --numeric-owner -C "$WORK" -cf - "$ARCHIVE_ROOT" \
    | gzip -n -9 > "$OUTPUT"
(
    cd "$DIST"
    sha256sum "$(basename "$OUTPUT")" > "$(basename "$OUTPUT").sha256"
    sha256sum --check "$(basename "$OUTPUT").sha256"
)
printf 'Created %s\n' "$OUTPUT"
