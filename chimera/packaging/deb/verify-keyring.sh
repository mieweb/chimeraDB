#!/usr/bin/env bash
# Accept only the pinned primary key and its subkeys, never a key bundle.
set -euo pipefail
[[ $# == 2 && $2 =~ ^[0-9A-F]{40}$ ]] || {
  echo 'usage: verify-keyring.sh KEYRING PRIMARY_FINGERPRINT' >&2; exit 1;
}
# A minimal runtime image has no ~/.gnupg. Do not read or create the caller's
# keyring/configuration just to inspect this downloaded public-key file.
key_home=$(mktemp -d)
trap 'rm -rf "$key_home"' EXIT
gpg --batch --no-options --homedir "$key_home" --show-keys --with-colons "$1" |
  awk -F: -v expected="$2" '
    $1 == "pub" { primary_count++; primary_pending=1; next }
    $1 == "sub" { primary_pending=0; next }
    $1 == "fpr" && primary_pending {
      primary_fingerprint=$10; primary_pending=0
    }
    END {
      if (primary_count != 1 || primary_fingerprint != expected) {
        print "repository keyring must contain exactly the pinned primary key" > "/dev/stderr"
        exit 1
      }
    }
  '
