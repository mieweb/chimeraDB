#!/usr/bin/env bash
# Review regressions that deliberately differ from MongoDB or depend on the
# local oplog pruner. Run only against a development/test instance: this trims
# its oplog history. Retention settings are restored even on failure.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"
chimera_require_running

mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval \
    "var d = db.getSiblingDB('csregression'); $1"
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
watch_pid=""
cleanup() {
  if [[ -n $watch_pid ]]; then
    kill "$watch_pid" 2>/dev/null || true
    wait "$watch_pid" 2>/dev/null || true
  fi
  chimera_sql -e "SET GLOBAL chimera_mongo_oplog_max_rows = $old_max_rows;
                  SET GLOBAL chimera_mongo_oplog_max_age_seconds = $old_max_age" || true
}
trap cleanup EXIT
chimera_sql -e "SET GLOBAL chimera_mongo_oplog_max_rows = 2;
                SET GLOBAL chimera_mongo_oplog_max_age_seconds = 0"

head=$(sql_scalar "SELECT MAX(seq) FROM chimera_meta.oplog")
watch_out="$INSTANCE_DIR/changestream-park-regression.txt"
mongo_eval '
  var s = d.runCommand({aggregate: "watched", pipeline: [{$changeStream: {}}], cursor: {}});
  assert.commandWorked(s);
  print("READY");
  // Keep one getMore in flight across a real background prune. Writes below
  // target another namespace, so no matching event can end this call early.
  var r = d.runCommand({getMore: s.cursor.id, collection: "watched", maxTimeMS: 30000});
  d.runCommand({killCursors: "watched", cursors: [s.cursor.id]});
  assert.eq(0, r.ok, tojson(r));
  assert.eq(286, r.code, tojson(r));
  assert.eq("ChangeStreamHistoryLost", r.codeName, tojson(r));
  print("  ok  in-flight getMore reports 286 after a background prune");
' >"$watch_out" 2>&1 &
watch_pid=$!

ready=false
for _ in $(seq 1 100); do
  if grep -q '^READY$' "$watch_out"; then ready=true; break; fi
  kill -0 "$watch_pid" 2>/dev/null || { cat "$watch_out"; die "watcher exited before opening"; }
  sleep 0.1
done
$ready || die "watcher did not open within 10 seconds"
# The entry history check and first empty read must finish before the pressure
# write; otherwise a test of the old implementation could pass accidentally.
sleep 1
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
  assert.commandWorked(d.runCommand({insert: "watched", documents: [{_id: "after-prune"}]}));
' >/dev/null
if ! wait "$watch_pid"; then
  watch_pid=""
  cat "$watch_out"
  die "getMore failed to report the lost history"
fi
watch_pid=""
sed '/^READY$/d' "$watch_out"
note "change-stream review regressions passed on $SERVER_VERSION"
