#!/usr/bin/env bash
# The other half of M5's exit criteria: a bidirectional projection column.
#
# `chimera_add_projection` lifts one document field into a real SQL column. A
# write to either representation updates the other, and — because reconciliation
# happens in a BEFORE trigger while the oplog is written by an AFTER trigger —
# a SQL `UPDATE` of the column produces exactly one oplog entry carrying the
# already-merged document.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
chimera_require_running

DB=projdemo
COLL=parts
NS="$DB.$COLL"

note "=== projection demo on $SERVER_VERSION ==="

mongo_eval() {
  "$ORACLE_MONGO" --quiet --port "$MONGO_PORT" --eval "$1"
}

# Rows are addressed by the document field rather than by the _id column, whose
# bytes are a BSON type tag followed by the key — an encoding a SQL client
# should not have to reproduce just to find a row.
ROW="WHERE JSON_VALUE(doc, '\$._id') = 'p1'"

READ_QTY="var r = db.getSiblingDB('$DB').runCommand({find: '$COLL', filter: {}});
          print(r.cursor.firstBatch[0].qty);"

mongo_eval "var d = db.getSiblingDB('$DB');
            d.runCommand({drop: '$COLL'});
            d.runCommand({insert: '$COLL', documents: [{_id: 'p1', sku: 'bolt', qty: 3}]});" \
  > /dev/null

note "adding a bidirectional projection of 'qty' onto column qty_col"
chimera_sql -e "CALL chimera_meta.chimera_add_projection('$NS', 'qty', 'qty_col', 'INT', 'bidirectional')"

backfilled=$(chimera_sql -N -B -e "SELECT qty_col FROM \`$DB\`.\`$COLL\` $ROW")
check_eq "backfilled column" "$backfilled" "3"

# Everything from here is measured against the oplog, so record where it stands.
before=$(chimera_sql -N -B -e "SELECT COALESCE(MAX(seq), 0) FROM chimera_meta.oplog")

note "SQL writes the column; the document must follow"
chimera_sql -e "UPDATE \`$DB\`.\`$COLL\` SET qty_col = 7 $ROW"

after_sql=$(mongo_eval "$READ_QTY")
check_eq "document after the SQL write" "$after_sql" "7"

# One entry, not two: the BEFORE trigger had already merged the document by the
# time the AFTER trigger read it.
produced=$(chimera_sql -N -B -e "SELECT COUNT(*) FROM chimera_meta.oplog WHERE seq > $before")
check_eq "oplog entries produced by the SQL write" "$produced" "1"

merged=$(chimera_sql -N -B -e "SELECT COUNT(*) FROM chimera_meta.oplog
                               WHERE seq > $before AND op = 'u'
                                 AND JSON_VALUE(o, '\$.qty.\"\$numberInt\"') = '7'")
check_eq "and it carries the merged document" "$merged" "1"

note "the wire writes the document; the column must follow"
mongo_eval "db.getSiblingDB('$DB').runCommand({update: '$COLL',
              updates: [{q: {_id: 'p1'}, u: {\$set: {qty: 11}}}]});" > /dev/null

after_wire=$(chimera_sql -N -B -e "SELECT qty_col FROM \`$DB\`.\`$COLL\` $ROW")
check_eq "column after the wire write" "$after_wire" "11"

note "projection demo passed on $SERVER_VERSION"
