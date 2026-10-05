#!/usr/bin/env bash
# Review regressions that deliberately differ from MongoDB or depend on the
# local oplog pruner. Run only against a development/test instance: this trims
# its oplog history and temporarily enables general query logging. Retention
# and logging settings are restored even on failure; existing logs are kept.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"
chimera_require_running
PARK_COLL="park_$$_${RANDOM}"

mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval \
    "var d = db.getSiblingDB('csregression'), PARK_COLL = '$PARK_COLL'; $1"
}

note "change-stream review regressions on $SERVER_VERSION"
mongo_eval '
  var r = d.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}});
  assert.eq(0, r.ok, tojson(r));
  assert.eq(238, r.code, tojson(r));
  assert(r.errmsg.indexOf("database-level watch") >= 0, tojson(r));
  assert(r.errmsg.indexOf("changestream-plan.md") >= 0, tojson(r));
  print("  ok  database-wide watch returns the documented NotImplemented error");

  d.runCommand({drop: "watched"});
  d.runCommand({drop: "pressure"});
  assert.commandWorked(d.runCommand({create: "watched"}));
  assert.commandWorked(d.runCommand({create: "pressure"}));
  assert.commandWorked(d.runCommand({insert: "watched", documents: [{_id: "seed"}]}));
'

old_max_rows=$(sql_scalar "SELECT @@GLOBAL.chimera_mongo_oplog_max_rows")
old_max_age=$(sql_scalar "SELECT @@GLOBAL.chimera_mongo_oplog_max_age_seconds")
old_general_log=$(sql_scalar "SELECT @@GLOBAL.general_log")
old_log_output=$(sql_scalar "SELECT @@GLOBAL.log_output")
watch_pid=""
cleanup() {
  if [[ -n $watch_pid ]]; then
    kill "$watch_pid" 2>/dev/null || true
    wait "$watch_pid" 2>/dev/null || true
  fi
  chimera_sql -e "SET GLOBAL chimera_mongo_oplog_max_rows = $old_max_rows;
                  SET GLOBAL chimera_mongo_oplog_max_age_seconds = $old_max_age;
                  SET GLOBAL general_log = $old_general_log;
                  SET GLOBAL log_output = '$old_log_output'" || true
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
chimera_sql -e "SET GLOBAL chimera_mongo_oplog_max_rows = 2;
                SET GLOBAL chimera_mongo_oplog_max_age_seconds = 0"
# Preserve an existing FILE sink while adding TABLE for synchronization. Remove
# NONE from the temporary setting: it disables all sinks even alongside TABLE.
case $old_log_output in
  *FILE*) log_output=FILE,TABLE ;;
  *) log_output=TABLE ;;
esac
chimera_sql -e "SET GLOBAL log_output = '$log_output'; SET GLOBAL general_log = ON"

head=$(sql_scalar "SELECT MAX(seq) FROM chimera_meta.oplog")
watch_out="$INSTANCE_DIR/changestream-park-regression.txt"
mongo_eval '
  var s = d.runCommand({aggregate: PARK_COLL, pipeline: [{$changeStream: {}}], cursor: {}});
  assert.commandWorked(s);
  // Keep one getMore in flight across a real background prune. Writes below
  // target another namespace, so no matching event can end this call early.
  var r = d.runCommand({getMore: s.cursor.id, collection: PARK_COLL, maxTimeMS: 60000});
  d.runCommand({killCursors: PARK_COLL, cursors: [s.cursor.id]});
  assert.eq(0, r.ok, tojson(r));
  assert.eq(286, r.code, tojson(r));
  assert.eq("ChangeStreamHistoryLost", r.codeName, tojson(r));
  print("  ok  in-flight getMore reports 286 after a background prune");
' >"$watch_out" 2>&1 &
watch_pid=$!

# MariaDB logs internal SQL-service statements after they complete. A SECOND
# event SELECT for this unique namespace proves this particular getMore has
# already passed its first empty read and park. aggregate/open does not select
# events, and the observer's SELECT COUNT cannot match this anchored prefix.
# No production debug hook or guessed client-side sleep is needed.
parked=false
for _ in $(seq 1 100); do
  kill -0 "$watch_pid" 2>/dev/null || { cat "$watch_out"; die "watcher exited before parking"; }
  reads=$(sql_scalar "SELECT COUNT(*) FROM mysql.general_log
    WHERE command_type = 'Query' AND argument LIKE 'SELECT seq, CONCAT(%'
      AND LOCATE(CONCAT(' AND ns = ', QUOTE('csregression.$PARK_COLL')), argument) > 0")
  if ((reads >= 2)); then parked=true; break; fi
  sleep 0.1
done
$parked || die "the specific getMore did not complete two empty event reads within 10 seconds"
kill -0 "$watch_pid" 2>/dev/null || { cat "$watch_out"; die "getMore ended before the pressure write"; }
note "observed $reads completed event reads for the pending getMore on $PARK_COLL"
mongo_eval '
  assert.commandWorked(d.runCommand({insert: "pressure", documents: [
    {_id: "pressure-1"}, {_id: "pressure-2"}, {_id: "pressure-3"}, {_id: "pressure-4"}
  ]}));
' >/dev/null

pruned=false
for _ in $(seq 1 200); do
  if [[ $(sql_scalar "SELECT MIN(seq) > $head + 1 FROM chimera_meta.oplog") == 1 ]]; then
    pruned=true
    break
  fi
  sleep 0.1
done
$pruned || die "background pruner did not trim the four-row burst within 20 seconds"

# On the buggy implementation the old getMore would silently deliver this
# marker, advancing its token over the removed rows. The fixed one must fail.
mongo_eval '
  assert.commandWorked(d.runCommand({insert: PARK_COLL, documents: [{_id: "after-prune"}]}));
' >/dev/null
if ! wait "$watch_pid"; then
  watch_pid=""
  cat "$watch_out"
  die "getMore failed to report the lost history"
fi
watch_pid=""
cat "$watch_out"

# Use the real pruner so the retained edge and durable deletion watermark
# describe the same transaction. A sequence gap by itself proves nothing.
chimera_sql -e 'SET GLOBAL chimera_mongo_oplog_max_rows = 1'
pruned=false
for _ in $(seq 1 200); do
  if [[ $(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog') == 1 ]]; then
    pruned=true
    break
  fi
  sleep 0.1
done
$pruned || die "background pruner did not retain only the newest marker within 20 seconds"
edge=$(sql_scalar "SELECT CONCAT(seq, ' ', ts_t, ' ', ts_i, ' ', ns)
                   FROM chimera_meta.oplog ORDER BY seq LIMIT 1")
read -r edge_seq edge_t edge_i edge_ns <<<"$edge"
[[ $edge_seq -gt 1 && $edge_ns == csregression.* ]] || die "unexpected retained edge: $edge"
edge_coll=${edge_ns#csregression.}
deleted=$(sql_scalar 'SELECT CONCAT(pruned_seq, " ", pruned_ts_t, " ", pruned_ts_i)
                     FROM chimera_meta.oplog_history WHERE id=1')
read -r deleted_seq deleted_t deleted_i <<<"$deleted"
[[ $deleted_seq -gt 0 && $deleted_seq -lt $edge_seq ]] || die "unexpected deletion watermark: $deleted"
"$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval \
  "var d = db.getSiblingDB('csregression'), EDGE_COLL = '$edge_coll',
       EDGE_SEQ = $edge_seq, EDGE_T = $edge_t, EDGE_I = $edge_i,
       DELETED_T = $deleted_t, DELETED_I = $deleted_i;"'
  var start = Timestamp(EDGE_T, EDGE_I);
  var s = d.runCommand({aggregate: EDGE_COLL,
                        pipeline: [{$changeStream: {startAtOperationTime: start}}], cursor: {}});
  assert.commandWorked(s);
  var r = d.runCommand({getMore: s.cursor.id, collection: EDGE_COLL, maxTimeMS: 0});
  assert.commandWorked(r);
  assert(r.cursor.nextBatch.length > 0, tojson(r));
  var expected = EDGE_SEQ.toString(16);
  while (expected.length < 16) expected = "0" + expected;
  assert.eq(expected, r.cursor.nextBatch[0]._id._data, tojson(r));
  d.runCommand({killCursors: EDGE_COLL, cursors: [s.cursor.id]});
  var lost = d.runCommand({aggregate: EDGE_COLL,
                           pipeline: [{$changeStream: {startAtOperationTime: Timestamp(DELETED_T, DELETED_I)}}], cursor: {}});
  assert.eq(286, lost.code, tojson(lost));
  print("  ok  retained-edge time replays its event; actually deleted time reports 286");
'
note "change-stream review regressions passed on $SERVER_VERSION"
