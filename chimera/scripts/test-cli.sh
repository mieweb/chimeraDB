#!/usr/bin/env bash
# Server-free tests of packaged CLI readiness and client argument forwarding.
# The fixture replaces only the mariadb executable; the real CLI runs unchanged.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CLI="$HERE/../cli/chimeradb"
[[ $# == 0 ]] || { echo 'usage: test-cli.sh' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/chimera-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
export FAKE_WORK="$work"
export FAKE_PLUGIN=ACTIVE FAKE_CATALOG=1 FAKE_UDF=1 FAKE_CONNECT=1
export FAKE_EXPECT_DEFAULTS="" FAKE_SOCKET='--socket=/tmp/chimera test.sock'
unset CHIMERA_DEFAULTS_FILE

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
[[ $# == 4 && $1 == -N && $2 == -B && $3 == -e ]] ||
  bad_argument 'unexpected SQL client argument shape'
case $4 in
  'SELECT VERSION()') echo '10.11.18-MariaDB' ;;
  "SELECT plugin_status FROM information_schema.plugins WHERE plugin_name = 'chimera_mongo'") printf '%s\n' "$FAKE_PLUGIN" ;;
  'SELECT @@chimera_mongo_port') echo 27017 ;;
  'SELECT @@chimera_mongo_bind') echo 127.0.0.1 ;;
  "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name = 'chimera_meta'") echo "$FAKE_CATALOG" ;;
  "SELECT COUNT(*) FROM mysql.func WHERE name = 'mongo'") echo "$FAKE_UDF" ;;
  *) bad_argument "unexpected query: $4" ;;
esac
FIXTURE
chmod +x "$work/bin/mariadb"
export PATH="$work/bin:$PATH"

checked=0
check_cli() {
  local expected=$1 label=$2 subcommand=${3:-status} actual=0
  : >"$work/calls"
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
  printf '  ok  %s\n' "$label"
}

check_cli 0 'active plugin with complete setup is ready'
export FAKE_PLUGIN=""
check_cli 1 'missing plugin fails even with complete setup'
grep -q 'not loaded' "$work/output"
export FAKE_PLUGIN=DISABLED
check_cli 1 'inactive plugin fails even with complete setup'
grep -q DISABLED "$work/output"
export FAKE_PLUGIN=ACTIVE FAKE_CATALOG=0
check_cli 1 'missing catalog fails with an active plugin'
export FAKE_CATALOG=1 FAKE_UDF=0
check_cli 1 'missing UDF fails with an active plugin'
export FAKE_UDF=1 FAKE_CONNECT=0
check_cli 1 'an unreachable SQL server is not ready'
export FAKE_CONNECT=1

# Homebrew's wrapper supplies this environment variable; the MariaDB client
# requires it before all other options. Paths with spaces also catch splitting.
export CHIMERA_DEFAULTS_FILE="$work/Homebrew config/my.cnf"
export FAKE_EXPECT_DEFAULTS="$CHIMERA_DEFAULTS_FILE"
check_cli 0 'status forwards defaults-file first and preserves client arguments'
check_cli 0 'setup forwards defaults-file first to every SQL invocation' setup
[[ -f $work/udf-created ]]
grep -q 'CREATE DATABASE IF NOT EXISTS chimera_meta' "$work/setup.sql"
printf 'CLI regressions: %s passed (no server needed)\n' "$checked"
