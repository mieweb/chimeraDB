#!/usr/bin/env bash
# Start a chimera MariaDB instance from its installed layout.
#
#   run-server.sh --server 10.11|11.8 [--fresh] [--plugin-load-add <name>]
#
# Initializes the datadir on first use, then starts mariadbd on the port this
# version owns (see chimeraDB-plan.md "Dev port conventions"). Runtime state
# lives under chimera/.run/<version>/ and is safe to delete.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

fresh=false
plugin_load=""
chimera_parse_server "$@"
set -- "${CHIMERA_ARGS[@]+"${CHIMERA_ARGS[@]}"}"
while (($#)); do
  case "$1" in
    --fresh) fresh=true; shift ;;
    --plugin-load-add) plugin_load="${2:-}"; shift 2 ;;
    *) die "unknown argument '$1'" ;;
  esac
done

chimera_require_dist

if chimera_is_running; then
  if $fresh; then
    "$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION"
  else
    note "already running (pid $(cat "$PIDFILE")) on port $SQL_PORT, socket $SOCKET"
    exit 0
  fi
fi

$fresh && rm -rf "$DATADIR"
mkdir -p "$INSTANCE_DIR"

if [[ ! -d $DATADIR ]]; then
  note "initializing datadir for $SERVER_VERSION"
  "$INSTALL_DB" --basedir="$SERVER_DIST" --datadir="$DATADIR" --auth-root-authentication-method=normal >"$INSTANCE_DIR/install-db.log" 2>&1 \
    || { tail -30 "$INSTANCE_DIR/install-db.log" >&2; die "mariadb-install-db failed (see $INSTANCE_DIR/install-db.log)"; }
fi

note "starting mariadbd $SERVER_VERSION on port $SQL_PORT"
"$MARIADBD" --no-defaults \
  --basedir="$SERVER_DIST" \
  --datadir="$DATADIR" \
  --port="$SQL_PORT" \
  --socket="$SOCKET" \
  --pid-file="$PIDFILE" \
  --log-error="$ERRLOG" \
  --bind-address=127.0.0.1 \
  ${plugin_load:+--plugin-load-add="$plugin_load"} \
  --plugin-dir="$SERVER_DIST/lib/plugin" \
  --user="$(id -un)" &

for _ in $(seq 1 60); do
  if chimera_sql -e 'SELECT 1' >/dev/null 2>&1; then
    note "$SERVER_VERSION ready — port $SQL_PORT, socket $SOCKET"
    exit 0
  fi
  sleep 1
done

tail -30 "$ERRLOG" >&2
die "server did not become ready (see $ERRLOG)"
