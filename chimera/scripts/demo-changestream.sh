#!/usr/bin/env bash
# The change-stream half of M5's promise, made executable: a `$changeStream`
# cursor sees writes made over the Mongo wire *and* writes made with the plain
# `mariadb` client, and it can be resumed from a token afterwards.
#
# This is the companion to demo-oplog.sh, and it exists for the assertions the
# differential suite cannot make. The reference build there resumes at the head
# of the stream instead of replaying the gap after a token, so replay — the thing
# a reconnecting client actually depends on — has to be checked against chimera
# directly rather than compared. Same for `operationTime`: there is nothing on
# the other side to compare a fence against.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
chimera_require_running

DB=csdemo
COLL=parts

note "=== change stream demo on $SERVER_VERSION (mongo port $MONGO_PORT, sql port $SQL_PORT) ==="

# The database and collection arrive as JS variables so that every probe below
# can be a single-quoted shell string. `$changeStream` then needs no escaping,
# which is the whole reason for the indirection.
mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" \
    --eval "var NS_DB = '$DB', NS_COLL = '$COLL'; $1"
}

mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  d.runCommand({drop: NS_COLL});
  d.runCommand({create: NS_COLL});
' > /dev/null

# Shell A: park on a change stream and print what arrives. It starts at the head,
# so it sees only the writes made below — a stream is a position, not a query.
WATCH_OUT="$RUN_DIR/$SERVER_VERSION/changestream-watch.txt"
mkdir -p "$(dirname "$WATCH_OUT")"

mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  var res = d.runCommand({aggregate: NS_COLL, pipeline: [{$changeStream: {}}], cursor: {}});
  var seen = 0;
  var deadline = new Date().getTime() + 20000;
  while (seen < 3 && new Date().getTime() < deadline) {
    var more = d.runCommand({getMore: res.cursor.id, collection: NS_COLL, maxTimeMS: 1000});
    more.cursor.nextBatch.forEach(function (e) {
      seen++;
      print("watch: " + e.operationType + " " + e.ns.db + "." + e.ns.coll + " " +
            tojson(e.documentKey, "", true));
    });
  }
  if (seen < 3) print("watch: TIMED OUT after " + seen + " events");
' > "$WATCH_OUT" 2>&1 &
WATCH_PID=$!

# Let the stream open and park before anything is written, so the order below is
# the order it observes.
sleep 1

note "shell B: two writes over the Mongo wire"
mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  d.runCommand({insert: NS_COLL, documents: [{_id: "wire-1", source: "wire"}]});
  d.runCommand({update: NS_COLL, updates: [{q: {_id: "wire-1"},
                                         u: {_id: "wire-1", source: "wire", seen: true}}]});
' > /dev/null

note "shell C: one write through the mariadb SQL client"
# The _id column holds the key's encoded form: a one-byte BSON type tag then the
# value, so a string and a number that print alike never collide. 0x02 is string.
chimera_sql -e "INSERT INTO \`$DB\`.\`$COLL\` (_id, doc)
                VALUES (CONCAT(0x02, 'sql-1'), '{\"_id\":\"sql-1\",\"source\":\"sql\"}')"

wait "$WATCH_PID" || true
cat "$WATCH_OUT"

if grep -q 'TIMED OUT' "$WATCH_OUT"; then
  die "the change stream did not observe all three writes"
fi
check_eq "events observed by the stream" "$(grep -c '^watch: ' "$WATCH_OUT")" "3"
check_eq "the third event is the raw-SQL write" \
  "$(sed -n '3p' "$WATCH_OUT" | grep -c 'sql-1')" "1"

note "a token replays the gap it names, so a reconnecting client misses nothing"
# The oplog outlives a single run of this script, so history is not read from the
# beginning of time — that would replay every previous run's events too. Instead
# a live stream is opened first and the token comes from inside this run, which
# is also how a real client comes by one.
replayed=$(mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  var live = d.runCommand({aggregate: NS_COLL, pipeline: [{$changeStream: {}}], cursor: {}});
  d.runCommand({insert: NS_COLL, documents: [{_id: "replay-1"}]});
  d.runCommand({update: NS_COLL, updates: [{q: {_id: "replay-1"}, u: {_id: "replay-1", n: 2}}]});
  d.runCommand({delete: NS_COLL, deletes: [{q: {_id: "replay-1"}, limit: 1}]});

  var seen = [];
  for (var i = 0; i < 10 && seen.length < 3; i++) {
    seen = seen.concat(d.runCommand({getMore: live.cursor.id, collection: NS_COLL,
                                     maxTimeMS: 500}).cursor.nextBatch);
  }
  // Resuming from the first of the three must bring back the other two, in order
  // and without repeating the one the token names.
  var again = d.runCommand({aggregate: NS_COLL,
                            pipeline: [{$changeStream: {startAfter: seen[0]._id}}],
                            cursor: {}});
  var back = [];
  for (var j = 0; j < 10 && back.length < 2; j++) {
    back = back.concat(d.runCommand({getMore: again.cursor.id, collection: NS_COLL,
                                     maxTimeMS: 500}).cursor.nextBatch);
  }
  print(back.length + " " + back.map(function (e) { return e.operationType; }));
')
check_eq "events replayed after the first token" "$replayed" "2 replace,delete"

note "a write reply names its own place in the stream, which is what a fence needs"
fenced=$(mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  var reply = d.runCommand({insert: NS_COLL, documents: [{_id: "fenced"}]});
  var s = d.runCommand({aggregate: NS_COLL,
                        pipeline: [{$changeStream: {startAtOperationTime: reply.operationTime}}],
                        cursor: {}});
  var got = [];
  for (var i = 0; i < 10 && got.length < 1; i++) {
    got = got.concat(d.runCommand({getMore: s.cursor.id, collection: NS_COLL,
                                   maxTimeMS: 500}).cursor.nextBatch);
  }
  print(got.length === 1 && got[0].documentKey._id === "fenced");
')
check_eq "the stream opened at a write's own time begins with that write" "$fenced" "true"

note "a resume point we no longer hold is refused by name, never skipped over"
lost=$(mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  print(d.runCommand({aggregate: NS_COLL,
                      pipeline: [{$changeStream: {resumeAfter: {_data: "0000000000000001"}}}],
                      cursor: {}}).codeName);
')
check_eq "a token behind the retained history" "$lost" "ChangeStreamHistoryLost"

note "abandoning a stream kills it like any other cursor"
gone=$(mongo_eval '
  var d = db.getSiblingDB(NS_DB);
  var s = d.runCommand({aggregate: NS_COLL, pipeline: [{$changeStream: {}}], cursor: {}});
  var killed = d.runCommand({killCursors: NS_COLL, cursors: [s.cursor.id]});
  print(killed.cursorsKilled.length + " " +
        d.runCommand({getMore: s.cursor.id, collection: NS_COLL}).codeName);
')
check_eq "getMore after killCursors" "$gone" "1 CursorNotFound"

note "the pruner never empties the oplog, so a resume question stays answerable"
# Age every entry past the retention limit and let one background pass run. The
# guard under test is that the newest row survives anyway: without it an idle
# system would prune itself to nothing and then answer ChangeStreamHistoryLost to
# a client that had missed nothing at all, because MIN(seq) is how that question
# is decided. This throws away oplog history, so it runs last.
chimera_sql -e "UPDATE chimera_meta.oplog SET ts_t = ts_t - 200000"
sleep 13
check_eq "rows left once every entry has aged out" \
  "$(chimera_sql -N -B -e "SELECT COUNT(*), MIN(seq) = MAX(seq) FROM chimera_meta.oplog" | tr '\t' ' ')" \
  "1 1"

note "change stream demo passed on $SERVER_VERSION"
