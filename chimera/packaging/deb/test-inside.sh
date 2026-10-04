#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && -d /packages ]] || {
  echo 'Run deb/test.sh; this script is only for its disposable container.' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive
die() { printf 'package test: %s\n' "$*" >&2; exit 1; }
package_version() {
  local directory=$1 plugin file package architecture version
  local plugins=("$directory/chimeradb-plugin-${SERIES}_"*.deb)
  [[ ${#plugins[@]} == 1 && -f ${plugins[0]} ]] ||
    die "expected exactly one plugin package for series $SERIES in $directory"
  plugin=${plugins[0]}
  version=$(dpkg-deb -f "$plugin" Version)
  # Validate every input before apt changes anything. Both sets must have the
  # requested series/architecture and a consistent package version.
  for file in "$directory"/*.deb; do
    package=$(dpkg-deb -f "$file" Package)
    architecture=$(dpkg-deb -f "$file" Architecture)
    case "$package" in
      chimeradb|chimeradb-common)
        [[ $architecture == all ]] || die "unexpected architecture in $file"
        ;;
      "chimeradb-plugin-$SERIES"|"chimeradb-plugin-$SERIES-dbgsym")
        [[ $architecture == "$ARCH" ]] || die "expected $ARCH package: $file"
        ;;
      *) die "unexpected package $package in $directory" ;;
    esac
    [[ $(dpkg-deb -f "$file" Version) == "$version" ]] ||
      die "mixed package versions in $directory"
  done
  printf '%s\n' "$version"
}
cd /packages
sha256sum --check SHA256SUMS
current_version=$(package_version /packages)
initial_packages=/packages
previous_version=
if [[ -d /previous-packages ]]; then
  (cd /previous-packages && sha256sum --check SHA256SUMS)
  previous_version=$(package_version /previous-packages)
  dpkg --compare-versions "$current_version" gt "$previous_version" ||
    die "current version $current_version must be newer than previous version $previous_version"
  initial_packages=/previous-packages
fi
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
apt-get update
apt-get install -y --no-install-recommends "$initial_packages"/*.deb
installed_version=$(dpkg-query -W -f='${Version}' "chimeradb-plugin-$SERIES")
[[ $installed_version == "${previous_version:-$current_version}" ]] || die 'wrong initial package version installed'
mkdir -p /tmp/chimera-data
chown mysql:mysql /tmp/chimera-data
mariadb-install-db --no-defaults --user=mysql --datadir=/tmp/chimera-data \
  --auth-root-authentication-method=normal > /tmp/install-db.log 2>&1
sql() { mariadb --socket=/tmp/chimera-test.sock --user=root -N -B "$@"; }
start_server() {
  mariadbd --user=mysql --datadir=/tmp/chimera-data \
    --socket=/tmp/chimera-test.sock --pid-file=/tmp/chimera-test.pid \
    --skip-networking --log-error=/tmp/chimera-test.log &
  server_pid=$!
  for ((i=0; i<120; i++)); do
    if sql -e 'SELECT 1' >/dev/null 2>&1; then return; fi
    kill -0 "$server_pid" 2>/dev/null || { cat /tmp/chimera-test.log; return 1; }
    sleep 1
  done
  cat /tmp/chimera-test.log
  return 1
}
stop_server() {
  mariadb-admin --socket=/tmp/chimera-test.sock --user=root shutdown
  wait "$server_pid"
}
start_server
chimeradb setup --socket=/tmp/chimera-test.sock --user=root
chimeradb setup --socket=/tmp/chimera-test.sock --user=root
[[ $(sql -e 'SELECT @@chimera_mongo_bind') == 127.0.0.1 ]]
sql -e 'CREATE DATABASE package_test'
sql package_test <<'SQL'
SELECT mongo('db.persist.insertOne({_id: "survives-upgrade", value: 42})');
SQL
[[ $(sql -e 'SELECT COUNT(*) FROM package_test.persist') == 1 ]]
stop_server

printf '\n# administrator setting retained across upgrade\n' >> /etc/mysql/mariadb.conf.d/60-chimera.cnf
if [[ -n $previous_version ]]; then
  apt-get install -y --no-install-recommends /packages/*.deb
  upgraded_version=$(dpkg-query -W -f='${Version}' "chimeradb-plugin-$SERIES")
  [[ $upgraded_version == "$current_version" ]] || die 'upgrade did not install the requested version'
  dpkg --compare-versions "$upgraded_version" gt "$installed_version" || die 'installed package version did not increase'
  grep -q 'administrator setting retained' /etc/mysql/mariadb.conf.d/60-chimera.cnf
  start_server
  chimeradb status --socket=/tmp/chimera-test.sock --user=root
  [[ $(sql -e "SELECT plugin_status FROM information_schema.plugins WHERE plugin_name='chimera_mongo'") == ACTIVE ]]
  [[ $(sql -e 'SELECT COUNT(*) FROM package_test.persist') == 1 ]]
  [[ $(sql package_test -e "SELECT mongo('db.persist.countDocuments({})')") == 1 ]]
  stop_server
  printf 'PASS: package version upgrade %s -> %s, active plugin, config and data preserved\n' "$installed_version" "$upgraded_version"
fi

# Retain the reinstall path as a separate check even after a version upgrade.
apt-get install -y --reinstall ./*.deb
grep -q 'administrator setting retained' /etc/mysql/mariadb.conf.d/60-chimera.cnf
start_server
chimeradb status --socket=/tmp/chimera-test.sock --user=root
[[ $(sql -e 'SELECT COUNT(*) FROM package_test.persist') == 1 ]]
stop_server

# Ordinary removal leaves conffiles. Their loose- options must not prevent the
# remaining MariaDB service from starting when chimera_mongo.so is gone.
apt-get remove -y chimeradb "chimeradb-plugin-$SERIES"
test -f /etc/mysql/mariadb.conf.d/60-chimera.cnf
start_server
[[ $(sql -e 'SELECT COUNT(*) FROM package_test.persist') == 1 ]]
stop_server
# The metapackage has already vanished completely (it owns no conffiles) and
# these local artifacts are not in an APT repository. Purge only the plugin's
# retained configuration and the still-installed common package.
apt-get purge -y "chimeradb-plugin-$SERIES" chimeradb-common
test ! -e /etc/mysql/mariadb.conf.d/60-chimera.cnf
start_server
[[ $(sql -e 'SELECT COUNT(*) FROM package_test.persist') == 1 ]]
stop_server
echo 'PASS: package install, setup idempotence, loopback default, reinstall, restart, remove, purge and data preservation'
