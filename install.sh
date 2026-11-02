#!/bin/sh
# Download a native gtiup manager; compiler selection remains explicit.
set -eu
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) platform=darwin-arm64 ;;
  Linux:x86_64) platform=linux-x64 ;;
  *) echo 'gtiup: supported hosts are macOS arm64 and Linux x64' >&2; exit 1 ;;
esac
case "${1-}" in
  --help|-h)
    echo 'Usage: install.sh [--default-toolchain preview|nightly|beta|stable|VERSION|none]'
    echo 'GTIUP_HOME selects the install directory (default ~/.gtiup).'
    echo 'GTIUP_VERSION=vX.Y.Z pins the manager download; default is latest.'
    exit 0 ;;
esac
if [ "$#" -ne 0 ] && { [ "$#" -ne 2 ] || [ "$1" != --default-toolchain ]; }; then
  echo 'gtiup: use --help for installer arguments' >&2; exit 1
fi
command -v curl >/dev/null 2>&1 || { echo 'gtiup: installer requires curl' >&2; exit 1; }
base=https://github.com/lang-gti-official/gtiup/releases/latest/download
if [ -n "${GTIUP_VERSION-}" ]; then
  printf '%s\n' "$GTIUP_VERSION" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'gtiup: invalid GTIUP_VERSION' >&2; exit 1; }
  base=https://github.com/lang-gti-official/gtiup/releases/download/$GTIUP_VERSION
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gtiup-install.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
asset=gtiup-$platform
fetch() { curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 "$base/$1" --output "$tmp/$1"; }
fetch "$asset"
fetch "$asset.sha256"
fetch "$asset.licenses.txt"
read -r expected filename < "$tmp/$asset.sha256"
[ "$filename" = "$asset" ] || { echo 'gtiup: unexpected checksum filename' >&2; exit 1; }
printf '%s\n' "$expected" | grep -Eq '^[0-9a-f]{64}$' || { echo 'gtiup: malformed checksum' >&2; exit 1; }
if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$tmp/$asset" | cut -d ' ' -f 1)
elif command -v shasum >/dev/null 2>&1; then
  actual=$(shasum -a 256 "$tmp/$asset" | cut -d ' ' -f 1)
else
  echo 'gtiup: installer requires sha256sum or shasum' >&2; exit 1
fi
[ "$actual" = "$expected" ] || { echo 'gtiup: download checksum mismatch' >&2; exit 1; }
chmod 755 "$tmp/$asset"
"$tmp/$asset" self-install "$@"
manager_home=${GTIUP_HOME:-"$HOME/.gtiup"}
mkdir -p "$manager_home/share/gtiup"
cp "$tmp/$asset.licenses.txt" "$manager_home/share/gtiup/licenses.txt"
printf '\nAdd this directory to your shell PATH: %s/bin\n' "$manager_home"
printf 'Your shell files were not modified. Use gtiup --help to choose a toolchain.\n'
