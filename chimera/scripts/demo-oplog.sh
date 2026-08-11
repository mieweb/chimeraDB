#!/usr/bin/env bash
# M5 exit criteria, made executable: a tailing cursor on local.oplog.rs sees
# writes made over the Mongo wire *and* writes made with the plain `mariadb`
# client, in commit order.
#
# That is the point of putting the oplog behind triggers. Nothing about the
# tail knows or cares which door a write came through.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
chimera_require_running

DB=oplogdemo
COLL=widgets
NS="$DB.$COLL"

note "=== oplog demo on $SERVER_VERSION (mongo port $MONGO_PORT, sql port $SQL_PORT) ==="

mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval "$1"
}

# A clean slate, and a collection that exists before the tail starts so the
# triggers are already in place.
mongo_eval "
  var d = db.getSiblingDB('$DB');
  d.runCommand({drop: '$COLL'});
  d.runCommand({create: '$COLL'});
" > /dev/null

# Shell A: park on a tailing, awaitData cursor and print whatever arrives. It
# starts from the current head, so it sees only the three writes below.
TAIL_OUT="$RUN_DIR/$SERVER_VERSION/oplog-tail.txt"
mkdir -p "$(dirname "$TAIL_OUT")"

mongo_eval "
  var l = db.getSiblingDB('local');
  // Meteor's opening move: ask for the newest entry to learn where 'now' is,
  // then tail everything strictly after it. Without this a tail replays the
  // whole oplog, on ChimeraDB exactly as on a replica set.
  var head = l.runCommand({find: 'oplog.rs', sort: {\$natural: -1}, limit: 1});
  var since = head.cursor.firstBatch.length ? head.cursor.firstBatch[0].ts : Timestamp(0, 0);

  var res = l.runCommand({find: 'oplog.rs', filter: {ns: '$NS', ts: {\$gt: since}},
                          tailable: true, awaitData: true, batchSize: 10});
  var id = res.cursor.id;
  var seen = 0;
  function report(entries) {
    entries.forEach(function (e) {
      seen++;
      print('tail: ' + e.op + ' ' + e.ns + ' ' + tojson(e.o, '', true));
    });
  }
  report(res.cursor.firstBatch);

  // Three writes are expected; awaitData parks for up to a second per call, so
  // this is bounded but generous if something is wrong.
  var deadline = new Date().getTime() + 20000;
  while (seen < 3 && new Date().getTime() < deadline) {
    var more = l.runCommand({getMore: id, collection: 'oplog.rs',
                             batchSize: 10, maxTimeMS: 1000});
    report(more.cursor.nextBatch);
  }
  if (seen < 3) print('tail: TIMED OUT after ' + seen + ' entries');
" > "$TAIL_OUT" 2>&1 &
TAIL_PID=$!

# Give the tail a moment to open its cursor and reach the parked state before
# anything is written, so the ordering below is the ordering it observes.
sleep 1

note "shell B: two writes over the Mongo wire"
mongo_eval "
  var d = db.getSiblingDB('$DB');
  d.runCommand({insert: '$COLL', documents: [{_id: 'wire-1', source: 'wire'}]});
  d.runCommand({update: '$COLL', updates: [{q: {_id: 'wire-1'}, u: {\$set: {seen: true}}}]});
" > /dev/null

note "shell C: one write through the mariadb SQL client"
# The _id column is the key's *encoded* form: a one-byte BSON type tag followed
# by the value, so that a string and a number that print alike never collide.
# 0x02 is the tag for a string.
chimera_sql -e "INSERT INTO \`$DB\`.\`$COLL\` (_id, doc)
                VALUES (CONCAT(0x02, 'sql-1'), '{\"_id\":\"sql-1\",\"source\":\"sql\"}')"

wait "$TAIL_PID" || true
cat "$TAIL_OUT"

if grep -q 'TIMED OUT' "$TAIL_OUT"; then
  die "the tail did not observe all three writes"
fi
check_eq "entries observed by the tail" "$(grep -c '^tail: ' "$TAIL_OUT")" "3"
check_eq "the third entry is the raw-SQL write" \
  "$(sed -n '3p' "$TAIL_OUT" | grep -c '"source" : "sql"')" "1"

note "oplog demo passed on $SERVER_VERSION"
