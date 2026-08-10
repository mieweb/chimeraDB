#!/usr/bin/env bash
# Stop a chimera MariaDB instance started by run-server.sh.
#
#   stop-server.sh --server 10.11|11.8

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"

if ! chimera_is_running; then
  note "$SERVER_VERSION not running"
  rm -f "$PIDFILE"
  exit 0
fi

pid=$(cat "$PIDFILE")
note "stopping mariadbd $SERVER_VERSION (pid $pid)"
kill "$pid"
for _ in $(seq 1 60); do
  kill -0 "$pid" 2>/dev/null || { note "stopped"; exit 0; }
  sleep 1
done

die "pid $pid still alive after 60s — inspect $ERRLOG before forcing"
