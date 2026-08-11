#!/usr/bin/env bash
# The fast inner loop for M6.2: which commands on Meteor's startup path does
# chimera not answer yet?
#
# A Meteor server will not serve a page until its Mongo driver has completed a
# handshake, checked whether it can use the oplog, and created the indexes its
# packages declare. Booting Meteor to discover a missing command costs a Node
# build; issuing the same commands directly costs a second.
#
# Every command below is one a real Meteor 3 server sends. The script reports
# rather than asserts: a gap list is the deliverable, not a pass/fail.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/_common.sh"
chimera_parse_server "$@"
chimera_require_running

note "=== Meteor startup-path probe on $SERVER_VERSION ==="

DB=meteor

# Each entry is "<label>|<database>|<command document>".
probes=(
  "hello|admin|{hello: 1}"
  "isMaster|admin|{isMaster: 1}"
  "buildInfo|admin|{buildInfo: 1}"
  "getParameter featureCompatibilityVersion|admin|{getParameter: 1, featureCompatibilityVersion: 1}"
  "replSetGetStatus|admin|{replSetGetStatus: 1}"
  "ismaster on local|local|{isMaster: 1}"
  "listDatabases|admin|{listDatabases: 1}"
  "listCollections|$DB|{listCollections: 1}"
  "createIndexes|$DB|{createIndexes: 'links', indexes: [{key: {title: 1}, name: 'title_1'}]}"
  # After createIndexes, because `listIndexes` on a collection that does not yet
  # exist is a NamespaceNotFound in real MongoDB too — probing it first would
  # report chimera's correct answer as a gap.
  "listIndexes|$DB|{listIndexes: 'links'}"
  "insert|$DB|{insert: 'links', documents: [{_id: 'probe', title: 'probe'}]}"
  "find|$DB|{find: 'links', filter: {}}"
  "count|$DB|{count: 'links'}"
  "aggregate count|$DB|{aggregate: 'links', pipeline: [{\$count: 'n'}], cursor: {}}"
  "update|$DB|{update: 'links', updates: [{q: {_id: 'probe'}, u: {\$set: {title: 'probe2'}}}]}"
  "delete|$DB|{delete: 'links', deletes: [{q: {_id: 'probe'}, limit: 1}]}"
  "oplog head|local|{find: 'oplog.rs', sort: {\$natural: -1}, limit: 1}"
  "oplog tail|local|{find: 'oplog.rs', filter: {}, tailable: true, awaitData: true, batchSize: 1}"
  "endSessions|admin|{endSessions: [{id: UUID()}]}"
)

missing=0
for probe in "${probes[@]}"; do
  label=${probe%%|*}
  rest=${probe#*|}
  dbname=${rest%%|*}
  cmd=${rest#*|}

  result=$("$REFERENCE_MONGO" --quiet --port "$MONGO_PORT" --eval "
    var r = db.getSiblingDB('$dbname').runCommand($cmd);
    print(r.ok == 1 ? 'ok' : (r.codeName || r.code || 'failed') + ': ' + (r.errmsg || ''));
  " 2>&1 | tr -d '\n')

  if [[ $result == ok ]]; then
    printf '  ok    %s\n' "$label"
  else
    printf '  GAP   %-40s %s\n' "$label" "$result"
    missing=$((missing + 1))
  fi
done

if ((missing == 0)); then
  note "no gaps on the Meteor startup path for $SERVER_VERSION"
else
  note "$missing gap(s) remain on the Meteor startup path for $SERVER_VERSION"
fi
