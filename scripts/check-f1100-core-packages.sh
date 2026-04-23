#!/usr/bin/env bash
set -euo pipefail

image="${1:-}"

if [ -z "$image" ] || [ ! -f "$image" ]; then
	echo "usage: $0 <f1100-sysupgrade-image>" >&2
	exit 2
fi

tmpdir="$(mktemp -d)"
cleanup() {
	rm -rf "$tmpdir"
}
trap cleanup EXIT

rootfs="$tmpdir/rootfs.squashfs"
extract_dir="$tmpdir/root"

offset="$(python3 - "$image" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
off = data.find(b'hsqs')
if off < 0:
    raise SystemExit(1)
print(off)
PY
)"

dd if="$image" of="$rootfs" bs=1 skip="$offset" status=none
unsquashfs -d "$extract_dir" "$rootfs" >/dev/null

required_packages=(
	cgi-io
	ethtool-full
	i2c-tools
	i2csfp
	kmod-bonding
	lm-sensors
	luci-base
	luci-app-firewall
	luci-app-package-manager
	proto-bonding
	rpcd-mod-luci
	sysfsutils
	uboot-envtools
)

if [[ "$image" == *"hasivo_f1100wp-4sx-4xgt"* ]]; then
	required_packages+=(kmod-pse-hasivo-hs104)
fi

missing=()
for pkg in "${required_packages[@]}"; do
	if [ ! -f "$extract_dir/lib/apk/packages/${pkg}.list" ]; then
		missing+=("$pkg")
	fi
done

if [ "${#missing[@]}" -ne 0 ]; then
	printf 'image %s is missing required F1100 packages:\n' "$image" >&2
	printf '  %s\n' "${missing[@]}" >&2
	exit 1
fi

printf 'image %s contains all required F1100 core packages\n' "$image"
