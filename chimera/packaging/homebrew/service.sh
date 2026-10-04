#!/usr/bin/env bash
# Foreground service used by launchd; also runnable against an isolated test prefix.
set -euo pipefail
: "${CHIMERA_MARIADB_PREFIX:?missing MariaDB prefix}"
: "${CHIMERA_PREFIX:?missing ChimeraDB prefix}"
: "${CHIMERA_DEFAULTS_FILE:?missing ChimeraDB config}"
: "${CHIMERA_DATA_DIR:?missing ChimeraDB data directory}"
export PATH="$CHIMERA_MARIADB_PREFIX/bin:$PATH"
die() { printf 'chimeradb service: %s\n' "$*" >&2; exit 1; }
expected=$(cat "$CHIMERA_PREFIX/share/chimeradb/mariadb-version")
actual=$(mariadbd --version | sed -E 's/.*(Ver|Distrib) ([0-9]+\.[0-9]+\.[0-9]+).*/\2/')
[[ $actual == "$expected" ]] || die "plugin was built for MariaDB $expected, installed $actual; run brew reinstall ${CHIMERA_BREW_FORMULA:-chimeradb}"
mkdir -p "$CHIMERA_DATA_DIR"
if [[ ! -d $CHIMERA_DATA_DIR/mysql ]]; then
  install_db="$CHIMERA_MARIADB_PREFIX/bin/mariadb-install-db"
  [[ -x $install_db ]] || install_db="$CHIMERA_MARIADB_PREFIX/scripts/mariadb-install-db"
  "$install_db" --no-defaults --basedir="$CHIMERA_MARIADB_PREFIX" \
    --datadir="$CHIMERA_DATA_DIR" --auth-root-authentication-method=socket \
    --auth-root-socket-user="$(id -un)" --skip-test-db
fi
mariadbd --defaults-file="$CHIMERA_DEFAULTS_FILE" &
server_pid=$!
cleanup() {
  kill -TERM "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' TERM INT
ready=false
for ((i=0; i<120; i++)); do
  kill -0 "$server_pid" 2>/dev/null || die 'MariaDB exited during startup; inspect the service error log'
  if mariadb --defaults-file="$CHIMERA_DEFAULTS_FILE" --protocol=socket --user="$(id -un)" -Nse 'SELECT 1' >/dev/null 2>&1; then
    ready=true
    break
  fi
  sleep 1
done
$ready || die 'MariaDB did not become ready within 120 seconds'
"$CHIMERA_PREFIX/libexec/chimeradb" setup --protocol=socket --user="$(id -un)"
wait "$server_pid"
