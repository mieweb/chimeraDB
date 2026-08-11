#!/usr/bin/env bash
# M7's exit criteria: the two cross-language gateways, in both directions.
#
#   * a mongo client running SQL — `{chimeraSql: "…"}` and the `$sql` stage
#   * a SQL client running mongo — `SELECT mongo('db.parts.find({})')`
#
# The point of the second one is not that it prints JSON. It is that the write
# it performs goes through the same command handlers a driver reaches, so it
# lands in the oplog as one ordinary entry — which is what the last check here
# actually measures.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
chimera_require_running

DB=gwdemo
COLL=parts

note "=== gateway demo on $SERVER_VERSION ==="

mongo_eval() {
  "$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval "$1"
}

# The function is created once per data directory and survives restarts; the
# IF NOT EXISTS keeps the demo replayable.
chimera_sql -e "CREATE FUNCTION IF NOT EXISTS mongo RETURNS STRING SONAME 'chimera_mongo.so'"
chimera_sql -e "SET GLOBAL chimera_mongo_sql_writes = OFF"

mongo_eval "var d = db.getSiblingDB('$DB');
            d.runCommand({drop: '$COLL'});
            d.runCommand({insert: '$COLL', documents: [
              {_id: 'p1', sku: 'bolt', qty: 3},
              {_id: 'p2', sku: 'nut', qty: 7}]});" > /dev/null

note "a mongo client runs SQL"
rows=$(mongo_eval "var r = db.getSiblingDB('$DB').runCommand(
         {chimeraSql: \"SELECT COUNT(*) AS n FROM \\\`$DB\\\`.\\\`$COLL\\\`\"});
       print(r.cursor.firstBatch[0].n.valueOf());")
check_eq "rows counted through chimeraSql" "$rows" "2"

stage=$(mongo_eval "var r = db.getSiblingDB('$DB').runCommand({aggregate: 1, cursor: {},
          pipeline: [{\$sql: \"SELECT JSON_VALUE(doc, '\\\$.sku') AS sku FROM \\\`$DB\\\`.\\\`$COLL\\\`\"},
                     {\$sort: {sku: 1}}, {\$limit: 1}]});
        print(r.cursor.firstBatch[0].sku);")
check_eq "the \$sql stage feeds the rest of the pipeline" "$stage" "bolt"

note "writes are refused until a DBA says otherwise"
refused=$(mongo_eval "var r = db.getSiblingDB('$DB').runCommand(
            {chimeraSql: \"DELETE FROM \\\`$DB\\\`.\\\`$COLL\\\`\"});
          print(r.codeName);")
check_eq "a write through chimeraSql without the flag" "$refused" "Unauthorized"

note "a SQL client runs mongo"
found=$(chimera_sql -N -B -e "USE $DB; SELECT mongo('db.$COLL.findOne({_id: \"p1\"})')")
# Canonical extJSON, exactly as the document is stored — including the fact that
# the legacy shell that inserted it types every number as a double.
check_eq "findOne through the mongo() function" "$found" \
  '{ "_id" : "p1", "sku" : "bolt", "qty" : { "$numberDouble" : "3.0" } }'

counted=$(chimera_sql -N -B -e "SELECT mongo('$DB', 'db.$COLL.countDocuments({})')")
check_eq "countDocuments with an explicit database" "$counted" "2"

# The same statement, quoted the other way round. Under ANSI_QUOTES the outer
# double quotes would be an identifier, so this is the form that works
# everywhere — and it works because the argument parser accepts either quote.
ansi=$(chimera_sql -N -B -e "SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES');
                             USE $DB;
                             SELECT mongo('db.$COLL.countDocuments({sku: \"nut\"})')")
check_eq "the single-quoted form under ANSI_QUOTES" "$ansi" "1"

# And the double-quoted outer form must fail there rather than half-work.
if chimera_sql -e "SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES');
                   USE $DB;
                   SELECT mongo(\"db.$COLL.countDocuments({})\")" > /dev/null 2>&1; then
  die "the double-quoted form should be an identifier under ANSI_QUOTES"
fi
note "  ok  the double-quoted form is refused under ANSI_QUOTES"

note "a write through mongo() reaches the oplog like any other write"
before=$(chimera_sql -N -B -e "SELECT COALESCE(MAX(seq), 0) FROM chimera_meta.oplog")
chimera_sql -e "USE $DB; SELECT mongo('db.$COLL.updateOne({_id: \"p1\"}, {\$set: {qty: 11}})')" \
  > /dev/null
after=$(chimera_sql -N -B -e "SELECT COUNT(*) FROM chimera_meta.oplog WHERE seq > $before")
check_eq "oplog entries produced by the mongo() write" "$after" "1"

wire_view=$(mongo_eval "var r = db.getSiblingDB('$DB').runCommand({find: '$COLL', filter: {_id: 'p1'}});
                        print(r.cursor.firstBatch[0].qty.valueOf());")
check_eq "the document a driver now sees" "$wire_view" "11"

note "gateway demo passed on $SERVER_VERSION"
