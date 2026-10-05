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
expected_prefix=$(cat "$CHIMERA_PREFIX/share/chimeradb/mariadb-prefix")
actual_prefix=$(cd "$CHIMERA_MARIADB_PREFIX" && pwd -P)
[[ $actual_prefix == "$expected_prefix" ]] || die "plugin was built for MariaDB keg $expected_prefix, installed $actual_prefix; run brew reinstall ${CHIMERA_BREW_FORMULA:-chimeradb}"
mkdir -p "$CHIMERA_DATA_DIR"
init_marker="$CHIMERA_DATA_DIR/.chimera-initializing"
require_complete_initialization() {
  [[ ! -e $init_marker && ! -L $init_marker ]] ||
    die "previous initialization did not finish: $init_marker exists; preserve the data directory for inspection, then restore a known-good backup or move it aside before retrying with an empty directory"
}
require_complete_initialization
child_pid=
cleanup() {
  if [[ -n $child_pid ]]; then
    kill -TERM "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
initializing=false
if [[ ! -d $CHIMERA_DATA_DIR/mysql ]]; then
  existing_entry=$(find "$CHIMERA_DATA_DIR" -mindepth 1 -maxdepth 1 -print -quit)
  [[ -z $existing_entry ]] || die "data directory is nonempty but has no mysql system tables; refusing to initialize over $CHIMERA_DATA_DIR"
  # Keep this marker after any failure or interruption, even if install-db has
  # already created mysql/. Only a completed catalog setup removes it.
  (set -o noclobber; : > "$init_marker") || die "could not create initialization marker: $init_marker"
  # Another initializer may have completed since our empty-directory snapshot.
  # We own this marker, but must not initialize over any data it left behind.
  existing_entry=$(find "$CHIMERA_DATA_DIR" -mindepth 1 -maxdepth 1 \
    ! -name .chimera-initializing -print -quit)
  if [[ -n $existing_entry ]]; then
    rm "$init_marker"
    die "data directory changed before initialization was claimed; refusing to initialize over $CHIMERA_DATA_DIR; inspect it before retrying"
  fi
  initializing=true
  install_db="$CHIMERA_MARIADB_PREFIX/bin/mariadb-install-db"
  [[ -x $install_db ]] || install_db="$CHIMERA_MARIADB_PREFIX/scripts/mariadb-install-db"
  "$install_db" --no-defaults --basedir="$CHIMERA_MARIADB_PREFIX" \
    --datadir="$CHIMERA_DATA_DIR" --auth-root-authentication-method=socket \
    --auth-root-socket-user="$(id -un)" --skip-test-db &
  child_pid=$!
  wait "$child_pid"
  child_pid=
else
  # A concurrent initializer can create mysql/ after our first marker check.
  require_complete_initialization
fi
mariadbd --defaults-file="$CHIMERA_DEFAULTS_FILE" &
server_pid=$!
child_pid=$server_pid
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
if $initializing; then
  kill -0 "$server_pid" 2>/dev/null || die 'MariaDB exited before initialization completed'
  rm "$init_marker"
fi
wait "$server_pid"
