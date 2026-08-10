#!/usr/bin/env bash
# Shared settings for every chimera script. Source it; never execute it.
#
# Single source of truth for: where each server tree lives, which ports it uses,
# and where runtime state goes. Nothing here hardcodes a path outside the repo.

set -euo pipefail

CHIMERA_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO_ROOT=$(cd "$CHIMERA_DIR/.." && pwd)

# CHIMERA_OUT moves every build product out of the checkout. Unset — the normal
# case — nothing moves. Set, a Linux container can build the same bind-mounted
# checkout the macOS host is already building without the two fighting over one
# cmake cache. The plugin's CMakeLists reads the same variable.
if [[ -n ${CHIMERA_OUT:-} ]]; then
  RUN_DIR="$CHIMERA_OUT/run"
  TRANSLATOR_BUILD="$CHIMERA_OUT/translator"
else
  RUN_DIR="$CHIMERA_DIR/.run"
  TRANSLATOR_BUILD="$CHIMERA_DIR/translator/build"
fi

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

# libbson is found through pkg-config. On Linux the system path already has it;
# on macOS it only exists under the Homebrew prefix, which is not searched by
# default.
chimera_export_pkg_config_path() {
  if command -v brew >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  fi
}

# Consumes "--server <10.11|11.8>" from the argument list and leaves everything
# else in CHIMERA_ARGS for the caller to parse. Sets the per-server variables.
chimera_parse_server() {
  local server=""
  CHIMERA_ARGS=()
  while (($#)); do
    case "$1" in
      --server) server="${2:-}"; shift 2 || die "--server needs a value" ;;
      --server=*) server="${1#*=}"; shift ;;
      *) CHIMERA_ARGS+=("$1"); shift ;;
    esac
  done
  [[ -n $server ]] || die "missing required --server 10.11|11.8"
  chimera_select_server "$server"
}

chimera_select_server() {
  case "$1" in
    10.11) SERVER_VERSION=10.11; SERVER_TREE="$REPO_ROOT/mariadb-10.11"; SQL_PORT=3308; MONGO_PORT=27019 ;;
    11.8)  SERVER_VERSION=11.8;  SERVER_TREE="$REPO_ROOT/mariadb-server"; SQL_PORT=3307; MONGO_PORT=27018 ;;
    *) die "unknown --server '$1' (expected 10.11 or 11.8)" ;;
  esac
  if [[ -n ${CHIMERA_OUT:-} ]]; then
    SERVER_BUILD="$CHIMERA_OUT/$SERVER_VERSION/build"
    SERVER_DIST="$CHIMERA_OUT/$SERVER_VERSION/dist"
  else
    SERVER_BUILD="$SERVER_TREE/build"
    SERVER_DIST="$SERVER_TREE/dist"
  fi
  INSTANCE_DIR="$RUN_DIR/$SERVER_VERSION"
  DATADIR="$INSTANCE_DIR/data"
  SOCKET="$INSTANCE_DIR/mysql.sock"
  PIDFILE="$INSTANCE_DIR/mariadbd.pid"
  ERRLOG="$INSTANCE_DIR/mariadbd.err"
  MARIADBD="$SERVER_DIST/bin/mariadbd"
  MARIADB="$SERVER_DIST/bin/mariadb"
  INSTALL_DB="$SERVER_DIST/scripts/mariadb-install-db"
  ORACLE_MONGO="$REPO_ROOT/mongodb/build/install/bin/mongo"
  ORACLE_MONGOD="$REPO_ROOT/mongodb/build/install/bin/mongod"
  ORACLE_MONGO_PORT=27117
}

chimera_require_dist() {
  [[ -x $MARIADBD ]] || die "$MARIADBD missing — run: cmake --install $SERVER_BUILD --prefix $SERVER_DIST"
}

# Run the client against this instance. Extra args are passed through.
chimera_sql() {
  "$MARIADB" --no-defaults --socket="$SOCKET" --user=root "$@"
}

chimera_is_running() {
  [[ -f $PIDFILE ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

chimera_require_running() {
  chimera_is_running || die "$SERVER_VERSION not running — start it with run-server.sh --server $SERVER_VERSION"
}

# --- assertions shared by every demo/test script -----------------------------

# Single scalar result of a query, unquoted and untabulated.
sql_scalar() { chimera_sql -N -B -e "$1"; }

check_eq() { # check_eq <label> <actual> <expected>
  [[ $2 == "$3" ]] || die "$1: expected '$3', got '$2'"
  printf '  ok  %s = %s\n' "$1" "$2"
}

check_sql_error() { # check_sql_error <label> <expected-errno> <sql>
  local out
  if out=$(chimera_sql -e "$3" 2>&1); then
    die "$1: expected ERROR $2, but the statement succeeded"
  fi
  [[ $out == *"ERROR $2"* ]] || die "$1: expected ERROR $2, got: $out"
  printf '  ok  %s rejected with ERROR %s\n' "$1" "$2"
}

