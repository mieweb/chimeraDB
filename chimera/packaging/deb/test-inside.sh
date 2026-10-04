#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && -d /packages ]] || {
  echo 'Run deb/test.sh; this script is only for its disposable container.' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive
cd /packages
sha256sum --check SHA256SUMS
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
apt-get update
apt-get install -y --no-install-recommends ./*.deb
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

# Reinstall exercises dpkg's upgrade/configuration path, including preserving a
# user-modified conffile. It is not a claim to test cross-version data migration.
printf '\n# administrator setting retained across upgrade\n' >> /etc/mysql/mariadb.conf.d/60-chimera.cnf
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
