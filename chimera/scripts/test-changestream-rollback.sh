#!/usr/bin/env bash
# A private, freshly initialized instance distinguishes rolled-back sequence
# allocations from committed history loss. Never resets the developer instance.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
MONGO_PORT=$((MONGO_PORT + 1000))
set -- "${CHIMERA_ARGS[@]+"${CHIMERA_ARGS[@]}"}"
while (($#)); do
  case "$1" in
    --mongo-port) [[ $# -ge 2 ]] || die "--mongo-port needs a value"; MONGO_PORT=$2; shift 2 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
[[ $MONGO_PORT =~ ^[0-9]+$ ]] && ((MONGO_PORT > 0 && MONGO_PORT < 65536)) || die "invalid Mongo port"
chimera_require_dist
[[ -x $REFERENCE_MONGO ]] || die "reference mongo shell missing: $REFERENCE_MONGO"
[[ -f $SERVER_DIST/lib/plugin/chimera_mongo.so ]] || die "build the Mongo plugin first"

# Keep Unix socket paths short enough for macOS, independent of checkout length.
INSTANCE_DIR=$(mktemp -d /tmp/chimera-csgap.XXXXXX)
DATADIR="$INSTANCE_DIR/data"
SOCKET="$INSTANCE_DIR/mysql.sock"
PIDFILE="$INSTANCE_DIR/mariadbd.pid"
ERRLOG="$INSTANCE_DIR/mariadbd.err"
server_pid=""
watch_pid=""
clock_locker_pid=""
clock_locker_id=""
stop_private() {
  [[ -n $server_pid ]] || return 0
  kill "$server_pid" 2>/dev/null || true
  for _ in $(seq 1 100); do
    kill -0 "$server_pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$server_pid" 2>/dev/null; then
    kill -KILL "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=""
    return 1
  fi
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
}
cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n $clock_locker_id ]]; then
    chimera_sql -e "KILL CONNECTION $clock_locker_id" >/dev/null 2>&1 || true
  fi
  if [[ -n $clock_locker_pid ]]; then
    kill "$clock_locker_pid" 2>/dev/null || true
    wait "$clock_locker_pid" 2>/dev/null || true
  fi
  if [[ -n $watch_pid ]]; then
    kill "$watch_pid" 2>/dev/null || true
    wait "$watch_pid" 2>/dev/null || true
  fi
  stop_private || status=1
  if ((status == 0)); then
    rm -rf "$INSTANCE_DIR"
  else
    printf 'Rollback fixture preserved for diagnosis: %s\n' "$INSTANCE_DIR" >&2
    [[ ! -f $ERRLOG ]] || tail -30 "$ERRLOG" >&2
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

start_private() {
  local mode=${1:-plugin}
  local plugin_args=()
  if [[ $mode == plugin ]]; then
    plugin_args=(--plugin-maturity=experimental --plugin-load-add=chimera_mongo
      --chimera-mongo-port="$MONGO_PORT" --chimera-mongo-bind=127.0.0.1
      --chimera-mongo-oplog-max-rows=0 --chimera-mongo-oplog-max-age-seconds=0)
  fi
  "$MARIADBD" --no-defaults --basedir="$SERVER_DIST" --datadir="$DATADIR" \
    --skip-networking --socket="$SOCKET" --pid-file="$PIDFILE" --log-error="$ERRLOG" \
    --plugin-dir="$SERVER_DIST/lib/plugin" --user="$(id -un)" \
    "${plugin_args[@]+"${plugin_args[@]}"}" >"$INSTANCE_DIR/server.out" 2>&1 &
  server_pid=$!
  local ready=false
  for _ in $(seq 1 100); do
    kill -0 "$server_pid" 2>/dev/null || die "private server exited during startup"
    if chimera_sql -e 'SELECT 1' >/dev/null 2>&1; then ready=true; break; fi
    sleep 0.1
  done
  $ready || die "private server did not become ready"
  if [[ $mode == plugin ]]; then
    check_eq "private plugin active" "$(sql_scalar "SELECT PLUGIN_STATUS FROM information_schema.plugins WHERE PLUGIN_NAME='chimera_mongo'")" ACTIVE
  fi
}
mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval \
    "load('$INSTANCE_DIR/assert-streams.js'); $1"
}
commit_event() {
  chimera_sql -e "INSERT INTO csgap.events (_id, doc)
    VALUES (CONCAT(0x02, '$1'), JSON_OBJECT('_id', '$1'))"
}
rollback_event() {
  sql_scalar "START TRANSACTION;
    INSERT INTO csgap.events (_id, doc)
      VALUES (CONCAT(0x02, '$1'), JSON_OBJECT('_id', '$1'));
    SELECT MAX(seq) FROM chimera_meta.oplog;
    ROLLBACK"
}
assert_unpruned() {
  check_eq "$1 history state" "$(sql_scalar 'SELECT CONCAT(pruned_seq, ":", pruned_ts_t, ":", pruned_ts_i, ":", legacy_baseline, ":", baseline_seq) FROM chimera_meta.oplog_history WHERE id=1')" '0:0:0:0:0'
}

cat >"$INSTANCE_DIR/assert-streams.js" <<'JS'
var d = db.getSiblingDB("csgap");
function token(seq) {
  var hex = seq.toString(16);
  while (hex.length < 16) hex = "0" + hex;
  return {_data: hex};
}
function replay(options, ids) {
  var s = d.runCommand({aggregate: "events", pipeline: [{$changeStream: options}], cursor: {}});
  assert.commandWorked(s);
  var r = d.runCommand({getMore: s.cursor.id, collection: "events", maxTimeMS: 0});
  assert.commandWorked(r);
  var batch = s.cursor.firstBatch.concat(r.cursor.nextBatch);
  assert.eq(ids, batch.map(function(e) { return e.documentKey._id; }), tojson(batch));
  assert.commandWorked(d.runCommand({killCursors: "events", cursors: [s.cursor.id]}));
}
function lost(options) {
  var r = d.runCommand({aggregate: "events", pipeline: [{$changeStream: options}], cursor: {}});
  assert.eq(0, r.ok, tojson(r));
  assert.eq(286, r.code, tojson(r));
  assert.eq("ChangeStreamHistoryLost", r.codeName, tojson(r));
}
JS
note "isolated rollback and pruning regressions on $SERVER_VERSION (Mongo $MONGO_PORT)"
"$INSTALL_DB" --no-defaults --basedir="$SERVER_DIST" --datadir="$DATADIR" \
  --auth-root-authentication-method=normal >"$INSTANCE_DIR/install-db.log" 2>&1 \
  || die "private initialization failed: $INSTANCE_DIR/install-db.log"
start_private
chimera_sql <"$CHIMERA_DIR/sql/catalog.sql"
mongo_eval 'assert.commandWorked(d.runCommand({create: "events"}));'
check_eq "fresh oplog empty" "$(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog')" 0
rolled_first=$(rollback_event rolled-before-first)
[[ $rolled_first -gt 0 ]] || die "rollback did not allocate an oplog sequence"
check_eq "rolled-back event absent" "$(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog')" 0
check_eq "rolled-back document absent" "$(sql_scalar 'SELECT COUNT(*) FROM csgap.events')" 0

# Exercise the public failure before inspecting the new schema, so this script
# also proves the old implementation fails on a genuine rolled-back allocation.
mongo_eval '
  var s = d.runCommand({aggregate: "events", pipeline: [{$changeStream: {}}], cursor: {}});
  assert.commandWorked(s);
  assert.eq(token(0), s.cursor.postBatchResumeToken, tojson(s));
  print("READY");
  var events = [], deadline = Date.now() + 15000;
  while (events.length === 0 && Date.now() < deadline) {
    var r = d.runCommand({getMore: s.cursor.id, collection: "events", maxTimeMS: 500});
    assert.commandWorked(r);
    events = events.concat(r.cursor.nextBatch);
  }
  assert.eq(["first"], events.map(function(e) { return e.documentKey._id; }), tojson(events));
  assert.commandWorked(d.runCommand({killCursors: "events", cursors: [s.cursor.id]}));
  print("  ok  head-at-zero stream crosses the initial rollback gap");
' >"$INSTANCE_DIR/head-watch.out" 2>&1 &
watch_pid=$!
ready=false
for _ in $(seq 1 100); do
  kill -0 "$watch_pid" 2>/dev/null || { cat "$INSTANCE_DIR/head-watch.out"; die "head watcher exited before READY"; }
  if grep -q '^READY$' "$INSTANCE_DIR/head-watch.out"; then ready=true; break; fi
  sleep 0.1
done
$ready || die "head watcher did not open within 10 seconds"
commit_event first
first=$(sql_scalar 'SELECT MAX(seq) FROM chimera_meta.oplog')
((first > rolled_first)) || die "first committed event did not follow the rollback gap"
if ! wait "$watch_pid"; then
  watch_pid=""; cat "$INSTANCE_DIR/head-watch.out"; die "head-at-zero stream failed across rollback gap"
fi
watch_pid=""
cat "$INSTANCE_DIR/head-watch.out"
mongo_eval 'replay({resumeAfter: token(0)}, ["first"]);
  replay({startAtOperationTime: Timestamp(0, 0)}, ["first"]);
  replay({}, []);'
assert_unpruned "first rollback"

rolled_inside=$(rollback_event rolled-between-events)
commit_event second
second=$(sql_scalar 'SELECT MAX(seq) FROM chimera_meta.oplog')
((rolled_inside > first && second > rolled_inside)) || die "interior rollback gap was not created"
check_eq "two committed documents" "$(sql_scalar 'SELECT COUNT(*) FROM csgap.events')" 2
mongo_eval "replay({resumeAfter: token($first)}, ['second']);
  replay({resumeAfter: token(0)}, ['first', 'second']);
  replay({startAtOperationTime: Timestamp(0, 0)}, ['first', 'second']);"
assert_unpruned "interior rollback"
stop_private || die "private shutdown timed out"
start_private
mongo_eval 'replay({resumeAfter: token(0)}, ["first", "second"]);
  replay({startAtOperationTime: Timestamp(0, 0)}, ["first", "second"]);'
assert_unpruned "restart before pruning"

# Hold the writer clock while observing a real no-op pruning pass. Its completed
# candidate read must appear before the clock is released: scanning retention
# while holding that lock would wait here and block every trigger writer too.
# max_rows counts two committed rows, not the four allocated sequence numbers.
chimera_sql -N -B --unbuffered -e 'START TRANSACTION;
  SELECT id FROM chimera_meta.oplog_clock WHERE id=1 FOR UPDATE;
  SELECT CONCAT("CLOCK_LOCKED:", CONNECTION_ID()); DO SLEEP(60); ROLLBACK' \
  >"$INSTANCE_DIR/clock-lock.out" 2>&1 &
clock_locker_pid=$!
for _ in $(seq 1 100); do
  clock_locker_id=$(sed -n 's/^CLOCK_LOCKED:\([0-9][0-9]*\)$/\1/p' "$INSTANCE_DIR/clock-lock.out")
  [[ -z $clock_locker_id ]] || break
  kill -0 "$clock_locker_pid" 2>/dev/null || die "clock-lock fixture exited early"
  sleep 0.1
done
[[ $clock_locker_id =~ ^[0-9]+$ ]] || die "clock-lock fixture did not acquire the writer clock"
chimera_sql -e 'SET GLOBAL log_output="TABLE"; SET GLOBAL general_log=ON;
  SET GLOBAL chimera_mongo_oplog_max_rows=2'
noop_query="SELECT seq FROM chimera_meta.oplog WHERE seq < $second AND (seq < $first) ORDER BY seq DESC LIMIT 1"
observed=false
for _ in $(seq 1 200); do
  if [[ $(sql_scalar "SELECT COUNT(*) FROM mysql.general_log WHERE command_type='Query' AND argument='$noop_query'") -gt 0 ]]; then
    observed=true; break
  fi
  sleep 0.1
done
$observed || die "no-op pruning waited on the writer clock instead of scanning outside it"
chimera_sql -e "KILL CONNECTION $clock_locker_id"
clock_locker_id=""
wait "$clock_locker_pid" 2>/dev/null || true
clock_locker_pid=""
# Taking the clock now also waits for any in-flight pruning transaction to
# finish before checking that a no-op never publishes a deletion watermark.
chimera_sql -e 'START TRANSACTION; SELECT id FROM chimera_meta.oplog_clock WHERE id=1 FOR UPDATE; COMMIT;
  SET GLOBAL general_log=OFF' >/dev/null
check_eq "row limit counts committed rows across gaps" "$(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog')" 2
assert_unpruned "no-op pruning"

chimera_sql -e 'SET GLOBAL chimera_mongo_oplog_max_rows=1'
pruned=false
for _ in $(seq 1 200); do
  if [[ $(sql_scalar 'SELECT pruned_seq FROM chimera_meta.oplog_history WHERE id=1') == "$first" ]]; then
    pruned=true; break
  fi
  sleep 0.1
done
$pruned || die "real pruning did not record the deleted event within 20 seconds"
check_eq "pruner retained the second event" "$(sql_scalar 'SELECT CONCAT(COUNT(*), ":", MIN(seq)) FROM chimera_meta.oplog')" "1:$second"
check_eq "pruning leaves collection data intact" "$(sql_scalar 'SELECT COUNT(*) FROM csgap.events')" 2
mongo_eval "lost({resumeAfter: token(0)}); lost({startAtOperationTime: Timestamp(0, 0)});
  replay({resumeAfter: token($first)}, ['second']);"
stop_private || die "private shutdown timed out"
start_private
check_eq "deleted-event watermark survives restart" "$(sql_scalar 'SELECT pruned_seq FROM chimera_meta.oplog_history WHERE id=1')" "$first"
mongo_eval "lost({resumeAfter: token(0)}); lost({startAtOperationTime: Timestamp(0, 0)});
  replay({resumeAfter: token($first)}, ['second']);"

# Recreate an old installation that has retained history but no watermark. Do
# the metadata removal with the plugin unloaded, eliminating a reseeding race.
# Retain TWO events so accepting only the newest event cannot pass this test.
commit_event third
edge=$(sql_scalar 'SELECT CONCAT(seq, " ", ts_t, " ", ts_i) FROM chimera_meta.oplog ORDER BY seq LIMIT 1')
read -r edge_seq edge_t edge_i <<<"$edge"
check_eq "two retained legacy events" "$(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog')" 2
stop_private || die "private shutdown timed out"
start_private plain
chimera_sql -e 'DROP TABLE chimera_meta.oplog_history'
stop_private || die "private shutdown timed out"
start_private
# Before the first legacy migration, an explicit gateway transaction can already
# hold the oplog clock and trigger-procedure metadata locks. Migration must tell
# that caller to finish its transaction, rather than wait on its own locks or
# implicitly commit it. Short private-server lock timeouts and an outer watchdog
# bound the broken implementation too; the fixed guard should return at once.
chimera_sql -e 'SET GLOBAL chimera_mongo_sql_writes=ON;
  SET GLOBAL lock_wait_timeout=2; SET GLOBAL innodb_lock_wait_timeout=2'
mongo_eval '
  function sql(statement) {
    var r = d.runCommand({chimeraSql: statement});
    assert.commandWorked(r);
    return r;
  }
  var missing = sql("SELECT COUNT(*) AS n FROM information_schema.tables " +
                    "WHERE table_schema=\"chimera_meta\" AND table_name=\"oplog_history\"");
  assert.eq(0, missing.cursor.firstBatch[0].n.valueOf(), "migration was seeded before the fixture");
  sql("BEGIN");
  try {
    sql("INSERT INTO csgap.events (_id, doc) VALUES " +
        "(CONCAT(0x02, \"migration-rollback\"), JSON_OBJECT(\"_id\", \"migration-rollback\"))");
    var own = sql("SELECT COUNT(*) AS n FROM chimera_meta.oplog " +
                  "WHERE JSON_VALUE(o, \"$._id\")=\"migration-rollback\"");
    assert.eq(1, own.cursor.firstBatch[0].n.valueOf(), "transaction did not fire the oplog trigger");
    var before = Date.now();
    var r = d.runCommand({aggregate: "events", pipeline: [{$changeStream: {}}], cursor: {}});
    assert.lt(Date.now() - before, 2000, "migration guard waited on the active transaction");
    assert.eq(0, r.ok, tojson(r));
    assert.eq(2, r.code, tojson(r));
    assert.eq("BadValue", r.codeName, tojson(r));
    assert(r.errmsg.indexOf("commit or roll back") >= 0, tojson(r));
    assert(r.errmsg.indexOf("then retry") >= 0, tojson(r));
    var active = sql("SELECT @@in_transaction AS active");
    assert.eq(1, active.cursor.firstBatch[0].active.valueOf(), "migration closed the transaction");
  } finally {
    sql("ROLLBACK");
  }
  print("  ok  legacy migration refuses an active transaction promptly and preserves rollback");
' >"$INSTANCE_DIR/migration-transaction.out" 2>&1 &
watch_pid=$!
finished=false
for _ in $(seq 1 100); do
  if ! kill -0 "$watch_pid" 2>/dev/null; then finished=true; break; fi
  sleep 0.1
done
$finished || die "migration transaction guard did not finish within 10 seconds"
if ! wait "$watch_pid"; then
  watch_pid=""; cat "$INSTANCE_DIR/migration-transaction.out"; die "migration transaction guard regression failed"
fi
watch_pid=""
cat "$INSTANCE_DIR/migration-transaction.out"
check_eq "migration probe document rolled back" "$(sql_scalar 'SELECT COUNT(*) FROM csgap.events WHERE _id=CONCAT(0x02, "migration-rollback")')" 0
check_eq "migration probe event rolled back" "$(sql_scalar 'SELECT COUNT(*) FROM chimera_meta.oplog WHERE JSON_VALUE(o, "$._id")="migration-rollback"')" 0
mongo_eval "replay({startAtOperationTime: Timestamp($edge_t, $edge_i)}, ['second', 'third']);
  replay({resumeAfter: token($edge_seq - 1)}, ['second', 'third']);
  lost({resumeAfter: token(0)}); lost({startAtOperationTime: Timestamp(0, 0)});"
check_eq "legacy baseline uses oldest retained event" "$(sql_scalar 'SELECT CONCAT(legacy_baseline, ":", baseline_seq, ":", baseline_ts_t, ":", baseline_ts_i) FROM chimera_meta.oplog_history WHERE id=1')" "1:$((edge_seq - 1)):$edge_t:$edge_i"
note "rollback gaps, no-op pruning, committed deletion, restart and legacy migration passed on $SERVER_VERSION"
