#!/usr/bin/env bash
# The upstream download is a bundle. Import it only into an isolated scratch
# keyring, export the pinned certificate, then verify exactly what APT will trust.
set -euo pipefail
[[ $# == 3 && $3 =~ ^[0-9A-F]{40}$ ]] || {
  echo 'usage: extract-keyring.sh DOWNLOADED_BUNDLE OUTPUT_KEYRING PRIMARY_FINGERPRINT' >&2
  exit 1
}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
key_home=$(mktemp -d)
trap 'rm -rf "$key_home"' EXIT
gpg --batch --no-options --no-autostart --homedir "$key_home" \
  --import-options import-minimal --import "$1"
gpg --batch --no-options --no-autostart --homedir "$key_home" \
  --export-options export-minimal --export "$3" >"$key_home/pinned.gpg"
"$HERE/verify-keyring.sh" "$key_home/pinned.gpg" "$3"
# Leave the caller's previous keyring untouched if import/export/verification
# fails, including a missing pin or a fingerprint matching only a subkey.
install -m 644 "$key_home/pinned.gpg" "$2"
