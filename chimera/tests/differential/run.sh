#!/usr/bin/env bash
# Differential test runner (M4.3).
#
# Every spec in specs/ is executed twice by the *same* mongo shell binary: once
# against a real mongod and once against ChimeraDB. The two transcripts are
# normalized and diffed. A spec passes only when they match, which makes MongoDB
# itself the oracle rather than our own expectations.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../../scripts/_common.sh"

chimera_parse_server "$@"
set -- ${CHIMERA_ARGS[@]+"${CHIMERA_ARGS[@]}"}

only=""
while [[ $# -gt 0 ]]; do
  case $1 in
    --only) only=${2:-}; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ -x $ORACLE_MONGO ]] || die "oracle shell missing: $ORACLE_MONGO"
[[ -x $ORACLE_MONGOD ]] || die "oracle mongod missing: $ORACLE_MONGOD"
chimera_require_running

WORK="$RUN_DIR/differential-$SERVER_VERSION"
ORACLE_DATA="$RUN_DIR/oracle/data"
ORACLE_LOG="$RUN_DIR/oracle/mongod.log"
rm -rf "$WORK"
mkdir -p "$WORK" "$ORACLE_DATA" "$(dirname "$ORACLE_LOG")"

oracle_pid=""
stop_oracle() {
  [[ -n $oracle_pid ]] || return 0
  kill "$oracle_pid" 2>/dev/null || true
  wait "$oracle_pid" 2>/dev/null || true
  oracle_pid=""
}
trap stop_oracle EXIT

note "starting oracle mongod on port $ORACLE_MONGO_PORT"
"$ORACLE_MONGOD" --dbpath "$ORACLE_DATA" --port "$ORACLE_MONGO_PORT" \
  --bind_ip 127.0.0.1 --logpath "$ORACLE_LOG" --logappend --fork >/dev/null
oracle_pid=$(cat "$ORACLE_DATA/mongod.lock")
[[ -n $oracle_pid ]] || die "oracle mongod did not report a pid — see $ORACLE_LOG"

# Anything that legitimately differs between two independent servers: generated
# ids, wall-clock stamps, cursor handles and replication bookkeeping. Everything
# else must match byte for byte.
normalize() {
  sed -E \
    -e 's/ObjectId\("[0-9a-f]{24}"\)/ObjectId(<oid>)/g' \
    -e 's/"\$oid" : "[0-9a-f]{24}"/"$oid" : "<oid>"/g' \
    -e 's/ISODate\("[^"]*"\)/ISODate(<date>)/g' \
    -e 's/Timestamp\([0-9]+, [0-9]+\)/Timestamp(<ts>)/g' \
    -e 's/"id" : NumberLong\("?[0-9]+"?\)/"id" : NumberLong(<cursor>)/g' \
    -e '/clusterTime|operationTime|signature|keyId|\$db|lsid/d' \
    -e '/^$/d' \
    "$1"
}

run_spec() {
  local port=$1 script=$2 out=$3
  # A nonzero exit is part of the transcript: if one side throws and the other
  # does not, the diff has to show it.
  "$ORACLE_MONGO" --quiet --port "$port" difftest "$script" >"$out" 2>&1 || true
}

passed=0
failed=0
for spec in "$SCRIPT_DIR"/specs/*.js; do
  name=$(basename "$spec" .js)
  [[ -z $only || $name == *"$only"* ]] || continue

  combined="$WORK/$name.js"
  cat "$SCRIPT_DIR/prelude.js" "$spec" >"$combined"

  run_spec "$ORACLE_MONGO_PORT" "$combined" "$WORK/$name.oracle.raw"
  run_spec "$MONGO_PORT" "$combined" "$WORK/$name.chimera.raw"
  normalize "$WORK/$name.oracle.raw" >"$WORK/$name.oracle"
  normalize "$WORK/$name.chimera.raw" >"$WORK/$name.chimera"

  if diff -u "$WORK/$name.oracle" "$WORK/$name.chimera" >"$WORK/$name.diff"; then
    printf '  ok   %s\n' "$name"
    passed=$((passed + 1))
  else
    printf '  FAIL %s\n' "$name"
    sed 's/^/       /' "$WORK/$name.diff" | head -40
    failed=$((failed + 1))
  fi
done

stop_oracle
note "differential: $passed passed, $failed failed (transcripts in $WORK)"
[[ $failed -eq 0 ]] || die "differential suite failed on $SERVER_VERSION"
