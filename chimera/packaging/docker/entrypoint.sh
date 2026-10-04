#!/usr/bin/env bash
# New volumes are initialized once. Root remains socket-authenticated locally;
# MARIADB_ROOT_PASSWORD sets the password for root connections over TCP.
set -euo pipefail
die() { printf 'chimeradb: %s\n' "$*" >&2; exit 1; }
[[ ${1:-} != -* ]] || set -- mariadbd "$@"
if [[ ${1:-} != mariadbd ]]; then exec "$@"; fi
for arg in "$@"; do
  case $arg in
    --help|--version|-V) exec "$@" ;;
    --datadir|--datadir=*|--socket|--socket=*|--defaults*|--no-defaults|--user|--user=*|--chroot|--chroot=*)
      die "the image manages its data directory, socket, config and user; unsupported override: $arg" ;;
  esac
done
[[ $(id -u) == 0 ]] || die 'start with the image default user; the entrypoint drops the server to mysql'
datadir=/var/lib/mysql
socket=/run/mysqld/mysqld.sock
mkdir -p "$datadir" /run/mysqld
chown mysql:mysql /run/mysqld
sql() { mariadb --no-defaults --protocol=socket --socket="$socket" --user=root "$@"; }
if [[ -f $datadir/.chimera-initializing ]]; then
  die 'previous initialization was interrupted; inspect the volume before retrying'
fi
if [[ ! -d $datadir/mysql ]]; then
  if [[ -n ${MARIADB_ROOT_PASSWORD_FILE:-} ]]; then
    [[ -z ${MARIADB_ROOT_PASSWORD:-} ]] || die 'set MARIADB_ROOT_PASSWORD or MARIADB_ROOT_PASSWORD_FILE, not both'
    MARIADB_ROOT_PASSWORD=$(cat "$MARIADB_ROOT_PASSWORD_FILE")
  fi
  [[ -n ${MARIADB_ROOT_PASSWORD:-} ]] || die 'set MARIADB_ROOT_PASSWORD (or _FILE) for an empty volume'
  [[ -z $(find "$datadir" -mindepth 1 -maxdepth 1 ! -name lost+found -print -quit) ]] ||
    die 'data directory is nonempty but has no mysql system tables; refusing to initialize over it'
  chown mysql:mysql "$datadir"
  touch "$datadir/.chimera-initializing"
  mariadb-install-db --no-defaults --user=mysql --datadir="$datadir" \
    --auth-root-authentication-method=socket --skip-test-db >/dev/null
  # A partial initialization never qualifies as a healthy existing volume.
  "$@" --user=mysql --skip-networking --chimera-mongo-bind=127.0.0.1 \
    --chimera-mongo-insecure-bind=OFF &
  init_pid=$!
  stop_init() { kill -TERM "$init_pid" 2>/dev/null || true; wait "$init_pid" 2>/dev/null || true; }
  trap stop_init EXIT
  trap 'exit 143' TERM INT
  ready=false
  for ((attempt=0; attempt<120; attempt++)); do
    if sql -e 'SELECT 1' >/dev/null 2>&1; then ready=true; break; fi
    kill -0 "$init_pid" 2>/dev/null || die 'initialization server exited'
    sleep 1
  done
  $ready || die 'initialization server did not become ready'
  # SET PASSWORD's grammar requires a string literal. Disable backslash escapes
  # and double quotes so arbitrary password bytes cannot become SQL syntax.
  password_sql=${MARIADB_ROOT_PASSWORD//\'/\'\'}
  if ! sql 2>/dev/null <<SQL
SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES';
CREATE USER IF NOT EXISTS 'root'@'%';
SET PASSWORD FOR 'root'@'%' = PASSWORD('$password_sql');
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;
SQL
  then
    die 'SQL root account initialization failed (password-bearing SQL suppressed)'
  fi
  unset password_sql
  chimeradb setup --protocol=socket --socket="$socket" --user=root
  mariadb-admin --no-defaults --socket="$socket" --user=root shutdown
  wait "$init_pid"
  trap - EXIT TERM INT
  rm "$datadir/.chimera-initializing"
fi
unset MARIADB_ROOT_PASSWORD MARIADB_ROOT_PASSWORD_FILE
exec gosu mysql "$@"
