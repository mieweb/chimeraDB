#!/usr/bin/env bash
# Used by both the package builder and the runtime Docker image.
set -euo pipefail
series=${1:?usage: configure-repository.sh 10.11|11.8}
case "$series" in 10.11|11.8) ;; *) echo "unsupported MariaDB series: $series" >&2; exit 1 ;; esac
. /etc/os-release
[[ $ID == debian && $VERSION_CODENAME == bookworm ]] || {
  echo 'ChimeraDB packages currently target Debian 12 (bookworm).' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg
cat > /etc/apt/sources.list.d/chimera-sources.sources <<'EOF'
Types: deb-src
URIs: http://deb.debian.org/debian
Suites: bookworm bookworm-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb-src
URIs: http://deb.debian.org/debian-security
Suites: bookworm-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
if [[ $series == 11.8 ]]; then
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
