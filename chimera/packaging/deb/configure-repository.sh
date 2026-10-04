#!/usr/bin/env bash
# Used by both the package builder and the runtime Docker image.
set -euo pipefail
series=${1:?usage: configure-repository.sh 10.11|11.8}
case "$series" in 10.11|11.8) ;; *) echo "unsupported MariaDB series: $series" >&2; exit 1 ;; esac
. /etc/os-release
case "$ID:$VERSION_CODENAME:$series" in
  debian:bookworm:10.11|debian:bookworm:11.8|debian:trixie:11.8) ;;
  debian:trixie:10.11)
    echo 'MariaDB 10.11 has no Debian 13 repository; use a Debian 12 container or host.' >&2
    exit 1 ;;
  *) echo 'Supported targets: Debian 12 (bookworm), or Debian 13 (trixie) with MariaDB 11.8.' >&2
    exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
# Match the existing Debian entries exactly, including Signed-By. New images
# use debian-archive-keyring.pgp while older hosts use .gpg; both files may exist,
# but APT rejects different key filenames for the same URI/suite. Cloning the
# binary stanza also preserves its URL, components and inline signing keys.
source_config=$(mktemp -d)
trap 'rm -rf "$source_config"' EXIT
for file in /etc/apt/sources.list.d/*.sources; do
  [[ -f $file && $file != /etc/apt/sources.list.d/chimera-sources.sources ]] || continue
  awk -v suite="$VERSION_CODENAME" '
    BEGIN { RS=""; ORS="\n\n" }
    {
      types=""; uris=""; suites=""; enabled="yes"
      count=split($0, lines, "\n")
      for (i=1; i<=count; i++) {
        if (lines[i] ~ /^Types:/) types=lines[i]
        if (lines[i] ~ /^URIs:/) uris=lines[i]
        if (lines[i] ~ /^Suites:/) suites=lines[i]
        if (lines[i] ~ /^Enabled:[ \t]*no[ \t]*$/) enabled="no"
      }
      if (enabled == "no" || types !~ /[ \t]deb([ \t]|$)/ || types ~ /[ \t]deb-src([ \t]|$)/) next
      if (uris !~ /https?:\/\/(deb\.debian\.org\/debian(-security)?|security\.debian\.org)(\/|[ \t]|$)/) next
      if (suites !~ ("[ \t]" suite "(-updates|-security)?([ \t]|$)")) next
      for (i=1; i<=count; i++) {
        if (lines[i] ~ /^Types:/) lines[i]="Types: deb-src"
        printf "%s\n", lines[i]
      }
      print ""
    }
  ' "$file" >> "$source_config/chimera-sources.sources"
done
# Older Debian hosts may still use one-line sources.list entries. Preserve the
# complete option block rather than guessing a signing key for those either.
for file in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
  [[ -f $file && $file != /etc/apt/sources.list.d/chimera-sources.list ]] || continue
  awk -v suite="$VERSION_CODENAME" '
    /^deb[ \t]/ && /https?:\/\/(deb\.debian\.org\/debian(-security)?|security\.debian\.org)(\/|[ \t]|$)/ {
      if ($0 ~ ("[ \t]" suite "(-updates|-security)?[ \t]")) {
        sub(/^deb[ \t]+/, "deb-src "); print
      }
    }
  ' "$file" >> "$source_config/chimera-sources.list"
done
touch "$source_config/chimera-sources.sources" "$source_config/chimera-sources.list"
install -m 644 "$source_config/chimera-sources.sources" /etc/apt/sources.list.d/chimera-sources.sources
install -m 644 "$source_config/chimera-sources.list" /etc/apt/sources.list.d/chimera-sources.list
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg
# Debian 12 provides 10.11; Debian 13 provides 11.8. Only the newer series on
# Debian 12 needs MariaDB.org, so native Debian 13 installs keep their own server
# packaging and its exact ABI identity rather than replacing it with a vendor build.
if [[ $VERSION_CODENAME == bookworm && $series == 11.8 ]]; then
  curl --fail --location --retry 3 --output /tmp/mariadb-keyring.gpg \
    https://supplychain.mariadb.com/mariadb-keyring-2019.gpg
  # Community signing-key fingerprint published by MariaDB; restrict trust to
  # this repository rather than adding a global trusted key.
  gpg --batch --show-keys --with-colons /tmp/mariadb-keyring.gpg |
    awk -F: '$1 == "fpr" { print $10 }' |
    grep -qx '177F4010FE56CA3336300305F1656F24C74CD1D8'
  install -m 644 /tmp/mariadb-keyring.gpg /usr/share/keyrings/chimera-mariadb.gpg
  cat > /etc/apt/sources.list.d/chimera-mariadb.sources <<'EOF'
Types: deb deb-src
URIs: https://deb.mariadb.org/11.8/debian
Suites: bookworm
Components: main
Signed-By: /usr/share/keyrings/chimera-mariadb.gpg
EOF
  cat > /etc/apt/preferences.d/chimera-mariadb <<'EOF'
Package: mariadb-* libmariadb* galera-4
Pin: origin deb.mariadb.org
Pin-Priority: 1000
EOF
fi
apt-get update
