#!/usr/bin/env bash
# Verify an installed/staged keg with fresh state, setup, SQL gateway and restart.
set -euo pipefail
die() { printf 'homebrew smoke: %s\n' "$*" >&2; exit 1; }
server= server_prefix= prefix= work=
while (($#)); do
  case "$1" in
    --server) server=${2:?}; shift 2 ;;
    --mariadb-prefix) server_prefix=${2:?}; shift 2 ;;
    --prefix) prefix=${2:?}; shift 2 ;;
    --work-dir) work=${2:?}; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ $server == 10.11 || $server == 11.8 ]] || die '--server 10.11|11.8 is required'
[[ -n $prefix && -n $server_prefix && -n $work ]] || die 'prefixes and --work-dir are required'
[[ ! -e $work/data ]] || die "$work/data already exists; use a fresh test directory"
mkdir -p "$work"
work=$(cd "$work" && pwd)
# Ports are overridable so both series can be exercised concurrently.
sql_port=${CHIMERA_TEST_SQL_PORT:-$((33060 + RANDOM % 500))}
mongo_port=${CHIMERA_TEST_MONGO_PORT:-$((27060 + RANDOM % 500))}
export CHIMERA_MARIADB_PREFIX=$server_prefix CHIMERA_PREFIX=$prefix
export CHIMERA_DATA_DIR=$work/data CHIMERA_DEFAULTS_FILE=$work/chimeradb.cnf
cat > "$CHIMERA_DEFAULTS_FILE" <<EOF
[client]
socket=$work/mysql.sock
[mysqld]
basedir=$server_prefix
datadir=$work/data
socket=$work/mysql.sock
pid-file=$work/mariadbd.pid
log-error=$work/mariadbd.err
bind-address=127.0.0.1
port=$sql_port
plugin-dir=$prefix/lib/chimeradb/plugin
plugin-maturity=experimental
plugin-load-add=chimera_mongo
chimera-mongo-bind=127.0.0.1
chimera-mongo-port=$mongo_port
EOF
service_pid=
cleanup() {
  if [[ -n $service_pid ]]; then
    kill -TERM "$service_pid" 2>/dev/null || true
    wait "$service_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
sql() { "$server_prefix/bin/mariadb" --defaults-file="$CHIMERA_DEFAULTS_FILE" --protocol=socket --user="$(id -un)" -N -B "$@"; }
start() {
  "$prefix/libexec/chimeradb-service" >> "$work/service.log" 2>&1 &
  service_pid=$!
  for ((i=0; i<120; i++)); do
    if ! kill -0 "$service_pid" 2>/dev/null; then
      cat "$work/service.log" >&2
      [[ ! -f $work/mariadbd.err ]] || cat "$work/mariadbd.err" >&2
      die 'service exited'
    fi
    if [[ $(sql -e "SELECT COUNT(*) FROM mysql.func WHERE name='mongo'" 2>/dev/null || true) == 1 ]]; then
      return
    fi
    sleep 1
  done
  die 'service setup timed out'
}
start
[[ $(sql -e 'SELECT @@chimera_mongo_bind') == 127.0.0.1 ]] || die 'listener is not bound to loopback'
[[ $(sql -e 'SELECT @@chimera_mongo_port') == "$mongo_port" ]] || die 'unexpected Mongo port'
sql -e 'CREATE DATABASE package_smoke; USE package_smoke; SELECT mongo('\''db.docs.insertOne({_id:"persisted",value:"brew"})'\'');' >/dev/null
[[ $(sql -e 'SELECT COUNT(*) FROM package_smoke.docs') == 1 ]] || die 'gateway insert did not reach SQL'
[[ $(sql -e "SELECT COUNT(*) FROM chimera_meta.oplog WHERE ns='package_smoke.docs'") == 1 ]] || die 'gateway insert did not reach oplog'
uri="mongodb://127.0.0.1:$mongo_port/?directConnection=true&serverSelectionTimeoutMS=5000"
"$prefix/libexec/chimeradb-wire-smoke" "$uri"
[[ $(sql -e 'SELECT COUNT(*) FROM package_smoke.wire_docs') == 1 ]] || die 'driver insert did not reach SQL'
cleanup
service_pid=
start
[[ $(sql -e 'USE package_smoke; SELECT mongo('\''db.docs.countDocuments({})'\'');') == 1 ]] || die 'document did not survive restart'
"$prefix/libexec/chimeradb-wire-smoke" "$uri" --ping-only
[[ $(sql -e 'SELECT COUNT(*) FROM package_smoke.wire_docs') == 1 ]] || die 'driver document did not survive restart'
printf 'Homebrew staging smoke passed: MariaDB %s, catalog, loopback, Mongo driver, gateway, oplog, restart persistence\n' "$server"
