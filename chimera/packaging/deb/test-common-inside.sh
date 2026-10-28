#!/usr/bin/env bash
# Common-only remote administration acceptance in a fresh disposable container.
set -euo pipefail
[[ -f /.dockerenv && -d /packages ]] || {
  echo 'Run deb/test.sh; this script is only for its disposable container.' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
. /etc/os-release
die() { printf 'common package test: %s\n' "$*" >&2; exit 1; }

# Validate the complete current set strictly, then select only its common
# package. Never enable the previous-release architecture exception here.
runtime_files=$(chimera-verify-packages /packages "$SERIES" "$ARCH" \
  "Debian $VERSION_ID ($VERSION_CODENAME)" --runtime-files)
common=
while IFS= read -r file; do
  if [[ $(dpkg-deb -f "$file" Package) == chimeradb-common ]]; then common=$file; fi
done <<<"$runtime_files"
[[ -n $common ]] || die 'validated current package set has no common package'
version=$(dpkg-deb -f "$common" Version)

no_local_server() {
  local unexpected
  unexpected=$(dpkg-query -W -f='${binary:Package}\t${Status}\n' | awk '
    $1 ~ /^(mariadb-server|mysql-server|default-mysql-server|chimeradb-plugin)(-|:|$)/ &&
    $0 !~ / not-installed$/ { print }')
  [[ -z $unexpected ]] || die "common-only install contains server/plugin packages: $unexpected"
  ! command -v mariadbd >/dev/null 2>&1 || die 'common-only install contains mariadbd'
  ! command -v mysqld >/dev/null 2>&1 || die 'common-only install contains mysqld'
}
no_local_server
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
apt-get update
apt-get install -y --no-install-recommends "$common"
no_local_server
[[ $(dpkg-query -W -f='${Version}' chimeradb-common) == "$version" ]] || die 'wrong common version installed'
[[ $(dpkg-query -W -f='${Architecture}' chimeradb-common) == "$ARCH" ]] || die 'common package is not the target architecture'
dpkg-query -L chimeradb-common | grep -Fxq /usr/libexec/chimeradb-health || die 'common package does not own its health helper'
[[ -x /usr/libexec/chimeradb-health ]] || die 'health helper is missing or not executable'

# Usage needs no peer, but executes the real binary and loads all of its shared
# libraries. A missing helper or undeclared library dependency cannot pass.
status=0
/usr/libexec/chimeradb-health >/tmp/health-usage.log 2>&1 || status=$?
[[ $status == 2 ]] || { cat /tmp/health-usage.log >&2; die "health helper usage exited $status, expected 2"; }
grep -q '^usage: chimeradb-health HOST PORT$' /tmp/health-usage.log || die 'unexpected helper usage response'
chimeradb --help >/tmp/chimeradb-help.log
grep -q 'chimeradb status' /tmp/chimeradb-help.log || die 'CLI help did not execute'
printf 'PASS: common-only %s/%s install owns a working CLI and health helper without a local MariaDB server or plugin\n' "$VERSION_CODENAME" "$ARCH"
