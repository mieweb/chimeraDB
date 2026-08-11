#!/usr/bin/env bash
# Differential test runner (M4.3).
#
# Every spec in specs/ is executed twice by the *same* mongo shell binary: once
# against a real mongod and once against ChimeraDB. The two transcripts are
# normalized and diffed. A spec passes only when they match, which makes MongoDB
# itself the reference rather than our own expectations.
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

[[ -x $REFERENCE_MONGO ]] || die "reference shell missing: $REFERENCE_MONGO"
[[ -x $REFERENCE_MONGOD ]] || die "reference mongod missing: $REFERENCE_MONGOD"
chimera_require_running

WORK="$RUN_DIR/differential-$SERVER_VERSION"
REFERENCE_DATA="$RUN_DIR/reference/data"
REFERENCE_LOG="$RUN_DIR/reference/mongod.log"
rm -rf "$WORK"
mkdir -p "$WORK" "$REFERENCE_DATA" "$(dirname "$REFERENCE_LOG")"

reference_pid=""
stop_reference() {
  [[ -n $reference_pid ]] || return 0
  kill "$reference_pid" 2>/dev/null || true
  wait "$reference_pid" 2>/dev/null || true
  reference_pid=""
}
trap stop_reference EXIT

note "starting reference mongod on port $REFERENCE_MONGO_PORT"
"$REFERENCE_MONGOD" --dbpath "$REFERENCE_DATA" --port "$REFERENCE_MONGO_PORT" \
  --bind_ip 127.0.0.1 --logpath "$REFERENCE_LOG" --logappend --fork >/dev/null
reference_pid=$(cat "$REFERENCE_DATA/mongod.lock")
[[ -n $reference_pid ]] || die "reference mongod did not report a pid — see $REFERENCE_LOG"

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
  "$REFERENCE_MONGO" --quiet --port "$port" difftest "$script" >"$out" 2>&1 || true
}

passed=0
failed=0
for spec in "$SCRIPT_DIR"/specs/*.js; do
  name=$(basename "$spec" .js)
  [[ -z $only || $name == *"$only"* ]] || continue

  combined="$WORK/$name.js"
  cat "$SCRIPT_DIR/prelude.js" "$spec" >"$combined"

  run_spec "$REFERENCE_MONGO_PORT" "$combined" "$WORK/$name.reference.raw"
  run_spec "$MONGO_PORT" "$combined" "$WORK/$name.chimera.raw"
  normalize "$WORK/$name.reference.raw" >"$WORK/$name.reference"
  normalize "$WORK/$name.chimera.raw" >"$WORK/$name.chimera"

  if diff -u "$WORK/$name.reference" "$WORK/$name.chimera" >"$WORK/$name.diff"; then
    printf '  ok   %s\n' "$name"
    passed=$((passed + 1))
  else
    printf '  FAIL %s\n' "$name"
    sed 's/^/       /' "$WORK/$name.diff" | head -40
    failed=$((failed + 1))
  fi
done

stop_reference
note "differential: $passed passed, $failed failed (transcripts in $WORK)"
[[ $failed -eq 0 ]] || die "differential suite failed on $SERVER_VERSION"
