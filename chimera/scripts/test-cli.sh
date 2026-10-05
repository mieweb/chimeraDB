#!/usr/bin/env bash
# Server-free tests of packaged CLI readiness and client argument forwarding.
# Fixtures replace the mariadb client and health helper; the CLI runs unchanged.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[[ $# == 0 ]] || { echo 'usage: test-cli.sh' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/chimera-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/package/bin" "$work/package/libexec" "$work/package/share/chimeradb/sql"
cp "$HERE/../cli/chimeradb" "$work/package/bin/"
cp "$HERE/../sql/catalog.sql" "$work/package/share/chimeradb/sql/"
CLI="$work/package/bin/chimeradb"
export FAKE_WORK="$work"
export FAKE_PLUGIN=ACTIVE FAKE_CATALOG=1 FAKE_UDF=1 FAKE_CONNECT=1
export FAKE_EXPECT_DEFAULTS="" FAKE_SOCKET='--socket=/tmp/chimera test.sock'
export FAKE_MONGO=1 FAKE_BIND=127.0.0.1 FAKE_PORT=27017
export FAKE_CONNECTION='Localhost via UNIX socket' FAKE_EXPECT_HOST=127.0.0.1 FAKE_EXPECT_PORT=27017
unset CHIMERA_DEFAULTS_FILE CHIMERA_MONGO_HOST CHIMERA_MONGO_PORT

cat >"$work/bin/mariadb" <<'FIXTURE'
#!/usr/bin/env bash
set -euo pipefail
bad_argument() {
  printf '%s\n' "$*" >>"$FAKE_WORK/fixture-errors"
  exit 2
}
if [[ -n $FAKE_EXPECT_DEFAULTS ]]; then
  [[ ${1:-} == "--defaults-file=$FAKE_EXPECT_DEFAULTS" ]] ||
    bad_argument 'defaults-file must be the first argument and preserve spaces'
  shift
else
  [[ ${1:-} != --defaults-file=* ]] || bad_argument 'unexpected defaults-file argument'
fi
[[ ${1:-} == "$FAKE_SOCKET" ]] || bad_argument 'client socket argument was not preserved'
shift
[[ ${1:-} == --user=root ]] || bad_argument 'client user argument was not preserved'
shift
printf 'called\n' >>"$FAKE_WORK/calls"
[[ $FAKE_CONNECT == 1 ]] || exit 1

if [[ $# == 0 ]]; then
  cat >"$FAKE_WORK/setup.sql"
  exit 0
fi
if [[ $# == 2 && $1 == -e && $2 == "CREATE FUNCTION IF NOT EXISTS mongo RETURNS STRING SONAME 'chimera_mongo.so'" ]]; then
  touch "$FAKE_WORK/udf-created"
  exit 0
fi
if [[ $# == 3 && $1 == -B && $2 == -e && $3 == '\s' ]]; then
  [[ ${LC_ALL:-} == C ]] || bad_argument 'connection status requires the C locale'
  printf 'Connection:\t\t%s\nPrivate client status detail\n' "$FAKE_CONNECTION"
  exit 0
fi
[[ $# == 4 && $1 == -N && $2 == -B && $3 == -e ]] ||
  bad_argument 'unexpected SQL client argument shape'
case $4 in
  'SELECT VERSION()') echo '10.11.18-MariaDB' ;;
  "SELECT plugin_status FROM information_schema.plugins WHERE plugin_name = 'chimera_mongo'") printf '%s\n' "$FAKE_PLUGIN" ;;
  'SELECT @@chimera_mongo_port') echo "$FAKE_PORT" ;;
  'SELECT @@chimera_mongo_bind') echo "$FAKE_BIND" ;;
  "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name = 'chimera_meta'") echo "$FAKE_CATALOG" ;;
  "SELECT COUNT(*) FROM mysql.func WHERE name = 'mongo'") echo "$FAKE_UDF" ;;
  *) bad_argument "unexpected query: $4" ;;
esac
FIXTURE
chmod +x "$work/bin/mariadb"
cat >"$work/package/libexec/chimeradb-health" <<'FIXTURE'
#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 || $1 != "$FAKE_EXPECT_HOST" || $2 != "$FAKE_EXPECT_PORT" ]]; then
  echo 'wrong Mongo endpoint arguments' >>"$FAKE_WORK/fixture-errors"
  exit 2
fi
printf 'called\n' >>"$FAKE_WORK/health-calls"
[[ $FAKE_MONGO == 1 ]] || { echo 'mock Mongo ping failed' >&2; exit 1; }
FIXTURE
chmod +x "$work/package/libexec/chimeradb-health"
cat >"$work/bin/systemctl" <<'FIXTURE'
#!/usr/bin/env bash
[[ $# == 2 && $1 == start && $2 == mariadb ]] || exit 2
touch "$FAKE_WORK/service-started"
FIXTURE
chmod +x "$work/bin/systemctl"
export PATH="$work/bin:$PATH"

checked=0
check_cli() {
  local expected=$1 label=$2 subcommand=${3:-status} actual=0
  : >"$work/calls"
  : >"$work/health-calls"
  if "$CLI" "$subcommand" "$FAKE_SOCKET" --user=root >"$work/output" 2>&1; then
    actual=0
  else
    actual=$?
  fi
  if [[ -s $work/fixture-errors || ! -s $work/calls || $actual != "$expected" ]]; then
    cat "$work/output" >&2
    [[ ! -f $work/fixture-errors ]] || cat "$work/fixture-errors" >&2
    echo "FAIL: $label (expected exit $expected, got $actual)" >&2
    exit 1
  fi
  checked=$((checked + 1))
  ! grep -q 'Private client status detail' "$work/output"
  printf '  ok  %s\n' "$label"
}

check_cli 0 'active plugin with successful Mongo ping and complete setup is ready'
[[ -s $work/health-calls ]]
export FAKE_MONGO=0
check_cli 1 'active plugin with dead Mongo listener is not ready'
check_cli 1 'service start fails readiness when plugin is active but Mongo is dead' start
[[ -f $work/service-started ]]
export FAKE_MONGO=1
export FAKE_PLUGIN=""
check_cli 1 'missing plugin fails even with complete setup'
[[ ! -s $work/health-calls ]]
grep -q 'not loaded' "$work/output"
export FAKE_PLUGIN=DISABLED
check_cli 1 'inactive plugin fails even with complete setup'
[[ ! -s $work/health-calls ]]
grep -q DISABLED "$work/output"
export FAKE_PLUGIN=ACTIVE FAKE_CATALOG=0
check_cli 1 'missing catalog fails with an active plugin'
export FAKE_CATALOG=1 FAKE_UDF=0
check_cli 1 'missing UDF fails with an active plugin'
export FAKE_UDF=1 FAKE_CONNECT=0
check_cli 1 'an unreachable SQL server is not ready'
export FAKE_CONNECT=1

export FAKE_BIND=0.0.0.0
check_cli 0 'wildcard bind on local SQL socket probes loopback'
export FAKE_BIND=127.0.0.1 FAKE_CONNECTION='database.example via TCP/IP' FAKE_EXPECT_HOST=database.example
check_cli 1 'remote SQL TCP requires its Mongo endpoint explicitly'
[[ ! -s $work/health-calls ]]
export CHIMERA_MONGO_HOST=database.example
check_cli 0 'explicit remote endpoint is probed even with a loopback Mongo bind'
unset CHIMERA_MONGO_HOST
export FAKE_CONNECTION='127.0.0.1 via TCP/IP'
check_cli 1 'loopback SQL TCP requires an explicit Mongo endpoint for tunnel safety'
[[ ! -s $work/health-calls ]]
export FAKE_CONNECTION='LOCALHOST via TCP/IP'
check_cli 1 'uppercase localhost cannot bypass explicit TCP endpoint selection'
[[ ! -s $work/health-calls ]]
export FAKE_CONNECTION='tunnel.example via TCP/IP'
check_cli 1 'a hostname alias cannot bypass explicit TCP endpoint selection'
[[ ! -s $work/health-calls ]]
export CHIMERA_MONGO_HOST=127.0.0.1 CHIMERA_MONGO_PORT=28000 FAKE_EXPECT_HOST=127.0.0.1 FAKE_EXPECT_PORT=28000
check_cli 0 'explicit Mongo host and forwarded port override endpoint detection'
unset CHIMERA_MONGO_HOST CHIMERA_MONGO_PORT
export FAKE_CONNECTION=unknown FAKE_EXPECT_PORT=27017
check_cli 1 'unknown SQL connection transport fails closed'
[[ ! -s $work/health-calls ]]
export FAKE_CONNECTION='Localhost via UNIX socket'
mv "$work/package/libexec/chimeradb-health" "$work/health-saved"
check_cli 1 'missing installed health helper cannot report ready'
mv "$work/health-saved" "$work/package/libexec/chimeradb-health"

# Source checkouts use the ignored build-health.sh output, including CHIMERA_OUT.
mkdir -p "$work/checkout/cli/health" "$work/checkout/.run/health"
cp "$CLI" "$work/checkout/cli/chimeradb"
touch "$work/checkout/cli/health/CMakeLists.txt"
cp "$work/package/libexec/chimeradb-health" "$work/checkout/.run/health/"
CLI="$work/checkout/cli/chimeradb"
check_cli 0 'checkout CLI finds the default ignored health build'
export CHIMERA_OUT="$work/custom build output"
mkdir -p "$CHIMERA_OUT/health"
cp "$work/package/libexec/chimeradb-health" "$CHIMERA_OUT/health/"
rm "$work/checkout/.run/health/chimeradb-health"
check_cli 0 'checkout CLI finds a custom CHIMERA_OUT health build'
unset CHIMERA_OUT
CLI="$work/package/bin/chimeradb"

# Homebrew's wrapper supplies this environment variable; the MariaDB client
# requires it before all other options. Paths with spaces also catch splitting.
export CHIMERA_DEFAULTS_FILE="$work/Homebrew config/my.cnf"
export FAKE_EXPECT_DEFAULTS="$CHIMERA_DEFAULTS_FILE"
check_cli 0 'status forwards defaults-file first and preserves client arguments'
check_cli 0 'setup forwards defaults-file first to every SQL invocation' setup
[[ -f $work/udf-created ]]
grep -q 'CREATE DATABASE IF NOT EXISTS chimera_meta' "$work/setup.sql"
printf 'CLI regressions: %s passed (no server needed)\n' "$checked"
