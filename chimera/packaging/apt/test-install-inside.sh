#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && ( -d /repository || -n ${CHIMERA_APT_URL:-} ) ]] || { echo 'Run test-install.sh' >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
case $VERSION_CODENAME in bookworm) series=10.11 ;; trixie) series=11.8 ;; *) exit 1 ;; esac
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg
if [[ -n ${CHIMERA_APT_URL:-} ]]; then
  bash /setup-repository.sh --repository-url "$CHIMERA_APT_URL" --key-fingerprint "$CHIMERA_APT_KEY"
else
install -d -m 755 /etc/apt/keyrings
install -m 644 /repository/chimeradb-archive-keyring.gpg /etc/apt/keyrings/chimeradb.gpg
cat > /etc/apt/sources.list.d/chimeradb.sources <<EOF
Types: deb
URIs: file:/repository
Suites: $VERSION_CODENAME
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/chimeradb.gpg
EOF
apt-get update --error-on=any
fi
# This must resolve all dependencies using repository metadata, not local .debs.
apt-get install -y --no-install-recommends chimeradb
[[ $(dpkg-query -W -f='${Status}' "chimeradb-plugin-$series") == 'install ok installed' ]]
if [[ -n ${CHIMERA_EXPECTED_VERSION:-} ]]; then
  [[ $(dpkg-query -W -f='${Version}' chimeradb) == "$CHIMERA_EXPECTED_VERSION" ]]
  [[ $(dpkg-query -W -f='${Version}' "chimeradb-plugin-$series") == "$CHIMERA_EXPECTED_VERSION" ]]
fi
mkdir -p /tmp/chimera-data
chown mysql:mysql /tmp/chimera-data
mariadb-install-db --no-defaults --user=mysql --datadir=/tmp/chimera-data \
  --auth-root-authentication-method=normal > /tmp/install-db.log 2>&1
mariadbd --user=mysql --datadir=/tmp/chimera-data --socket=/tmp/chimera-test.sock \
  --pid-file=/tmp/chimera-test.pid --skip-networking --log-error=/tmp/chimera-test.log &
server_pid=$!
cleanup() {
  mariadb-admin --socket=/tmp/chimera-test.sock --user=root shutdown || true
  wait "$server_pid" || true
}
trap cleanup EXIT
ready=false
for ((i=0; i<120; i++)); do
  if mariadb --socket=/tmp/chimera-test.sock --user=root -e 'SELECT 1' >/dev/null 2>&1; then
    ready=true
    break
  fi
  kill -0 "$server_pid" 2>/dev/null || { cat /tmp/chimera-test.log; exit 1; }
  sleep 1
done
$ready || { cat /tmp/chimera-test.log; exit 1; }
chimeradb setup --socket=/tmp/chimera-test.sock --user=root
chimeradb status --socket=/tmp/chimera-test.sock --user=root
mariadb --socket=/tmp/chimera-test.sock --user=root <<'SQL'
CREATE DATABASE apt_acceptance;
USE apt_acceptance;
SELECT mongo('db.persist.insertOne({_id: "from-apt", value: 42})');
SELECT mongo('db.persist.findOne({_id: "from-apt"})');
SQL
[[ $(mariadb --socket=/tmp/chimera-test.sock --user=root -N -B \
  -e 'SELECT COUNT(*) FROM apt_acceptance.persist') == 1 ]]
printf 'PASS: %s signed APT installation, dependencies, readiness and SQL/Mongo write\n' "$VERSION_CODENAME"
