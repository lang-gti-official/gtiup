#!/bin/sh
# Install the native manager and a complete GTI toolchain in one command.
set -eu
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) platform=darwin-arm64 ;;
  Linux:x86_64) platform=linux-x64 ;;
  *) echo 'gtiup: supported hosts are macOS arm64 and Linux x64' >&2; exit 1 ;;
esac
toolchain=nightly
modify_path=true
while [ "$#" -gt 0 ]; do
  case "$1" in
    --help|-h)
      echo 'Usage: install.sh [--default-toolchain nightly|beta|stable|VERSION|none] [--no-modify-path]'
      echo 'Installs nightly by default and configures PATH for bash, zsh and sh.'
      echo 'GTIUP_HOME selects the install directory (default ~/.gtiup).'
      echo 'GTIUP_VERSION=vX.Y.Z pins the manager download; default is latest.'
      exit 0 ;;
    --default-toolchain)
      [ "$#" -ge 2 ] || { echo 'gtiup: --default-toolchain needs a value' >&2; exit 1; }
      toolchain=$2; shift 2 ;;
    --no-modify-path) modify_path=false; shift ;;
    *) echo 'gtiup: use --help for installer arguments' >&2; exit 1 ;;
  esac
done
if [ "$toolchain" = preview ]; then
  echo 'gtiup: the preview channel is retired and no longer published; use --default-toolchain nightly' >&2; exit 1
fi
printf '%s\n' "$toolchain" | grep -Eq '^(nightly|beta|stable|none|[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?)$' || {
  echo 'gtiup: invalid toolchain selection' >&2; exit 1;
}
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
"$tmp/$asset" self-install --default-toolchain "$toolchain"
manager_home=${GTIUP_HOME:-"$HOME/.gtiup"}
mkdir -p "$manager_home/share/gtiup"
cp "$tmp/$asset.licenses.txt" "$manager_home/share/gtiup/licenses.txt"
# A piped installer runs in a child shell. Configure future shells, then tell
# the user to open a new terminal; do not pretend to alter the parent PATH.
manager_home=$(cd "$manager_home" && pwd -P)
quote() {
  printf "'"
  printf '%s' "$1" | sed "s/'/'\\\\''/g"
  printf "'"
}
login_shell=${SHELL:-/bin/sh}
if [ "$modify_path" = true ]; then
  case "$manager_home$HOME${ZDOTDIR-}" in
    *'
'*) echo 'gtiup: shell paths must not contain newlines; use --no-modify-path' >&2; exit 1 ;;
  esac
  case "${login_shell##*/}" in
    bash|zsh|sh|'') ;;
    *) echo 'gtiup: automatic PATH setup supports bash, zsh and sh; use --no-modify-path for other shells' >&2; exit 1 ;;
  esac
  env_file=$manager_home/env
  if [ -e "$env_file" ] || [ -L "$env_file" ]; then
    [ ! -L "$env_file" ] && [ "$(head -n 1 "$env_file")" = '# gtiup shell environment' ] || {
      echo "gtiup: refusing to overwrite unrelated $env_file" >&2; exit 1;
    }
  fi
  {
    echo '# gtiup shell environment'
    printf 'export GTIUP_HOME=%s\n' "$(quote "$manager_home")"
    printf 'case ":$PATH:" in\n  *:%s:*) ;;\n  *) export PATH=%s:"$PATH" ;;\nesac\n' "$(quote "$manager_home/bin")" "$(quote "$manager_home/bin")"
  } > "$tmp/env"
  cp "$tmp/env" "$env_file"
  line=". $(quote "$env_file")"
  add_profile() {
    mkdir -p "$(dirname "$1")"
    if [ ! -f "$1" ] || ! grep -Fqx -- "$line" "$1"; then
      printf '\n# gtiup\n%s\n' "$line" >> "$1"
    fi
  }
  add_profile "$HOME/.profile"
  case "${login_shell##*/}" in
    zsh) add_profile "${ZDOTDIR:-$HOME}/.zshenv" ;;
    bash)
      add_profile "$HOME/.bashrc"
      if [ -f "$HOME/.bash_profile" ]; then add_profile "$HOME/.bash_profile"
      elif [ -f "$HOME/.bash_login" ]; then add_profile "$HOME/.bash_login"; fi ;;
  esac
  printf '\nInstallation complete. Open a new terminal to use gti and gtiup.\n'
else
  printf '\nInstallation complete. Add this directory to PATH: %s/bin\n' "$manager_home"
fi
