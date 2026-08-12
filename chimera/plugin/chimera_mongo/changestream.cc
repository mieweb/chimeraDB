#include "changestream.h"

#include <cstdlib>
#include <string>

#include "chimera/codec.h"
#include "chimera/error.h"

namespace chimera {

namespace {

// One oplog row as a change event, assembled the same way kEntryExpr assembles
// an oplog entry and for the same reason: MariaDB's JSON is text, so JSON_OBJECT
// would embed `o` as a *string* rather than nest it.
//
// The mapping is regular enough to be one expression. Our 'u' rows carry the
// whole merged post-image (M5.2/M5.8), which is both why `fullDocument` is free
// and why the honest operationType for them is `replace` rather than `update`
// with an updateDescription we never computed.
const char kEventExpr[] =
    // HEX() of a BIGINT is uppercase and unpadded; the token codec's contract is
    // 16 lowercase digits, and decode_resume_token rejects anything else.
    "CONCAT('{\"_id\":{\"_data\":\"', LPAD(LOWER(HEX(seq)), 16, '0'),"
    " '\"},\"operationType\":\"',"
    " CASE op WHEN 'i' THEN 'insert' WHEN 'u' THEN 'replace' ELSE 'delete' END,"
    " '\",\"clusterTime\":{\"$timestamp\":{\"t\":', ts_t, ',\"i\":', ts_i,"
    " '}},\"wallTime\":{\"$date\":{\"$numberLong\":\"', ts_t * 1000, '\"}}',"
    // A delete has no post-image to report, and the field is absent rather than
    // null — same rule the oplog's own o2 follows.
    " IF(op = 'd', '', CONCAT(',\"fullDocument\":', o)),"
    // Stored `ns` splits at the *first* dot: a database name cannot contain one,
    // a collection name can.
    " ',\"ns\":{\"db\":', JSON_QUOTE(SUBSTRING_INDEX(ns, '.', 1)),"
    " ',\"coll\":', JSON_QUOTE(SUBSTRING(ns, LOCATE('.', ns) + 1)),"
    // 'u' already stores {_id} in o2; for 'i' and 'd' the key is in o, which for
    // a delete is nothing but the key anyway.
    " '},\"documentKey\":', COALESCE(o2, JSON_OBJECT('_id', JSON_EXTRACT(o, '$._id'))),"
    " '}')";

uint64_t scalar(SqlSession& sql, const std::string& statement) {
  ResultSet rows = sql.query(statement);
  if (rows.rows.empty() || !rows.rows[0][0]) return 0;
  return std::strtoull(rows.rows[0][0]->c_str(), nullptr, 10);
}

}  // namespace

OplogBatch read_changestream(SqlSession& sql, const Namespace& ns, uint64_t after_seq,
                             uint64_t limit) {
  ResultSet rows = sql.query(
      sql.render("SELECT seq, " + std::string(kEventExpr) +
                     " AS event FROM chimera_meta.oplog WHERE seq > " +
                     std::to_string(after_seq) + " AND ns = ? ORDER BY seq ASC LIMIT " +
                     std::to_string(limit),
                 {Param{ns.text()}}));

  OplogBatch batch;
  batch.last_seq = after_seq;
  for (const auto& row : rows.rows) {
    if (!row[0] || !row[1]) continue;
    batch.last_seq = std::strtoull(row[0]->c_str(), nullptr, 10);
    batch.documents.push_back(from_extjson(*row[1]));
  }
  return batch;
}

uint64_t resolve_change_stream_start(SqlSession& sql, const ChangeStreamOptions& opts) {
  switch (opts.start) {
    case ChangeStreamStart::kToken:
      return opts.after_seq;
    case ChangeStreamStart::kOperationTime:
      // "at or after" the requested time, so we resume after the last row that
      // is strictly before it. (t, i) compares lexicographically.
      return scalar(sql, "SELECT COALESCE(MAX(seq), 0) FROM chimera_meta.oplog WHERE ts_t < " +
                             std::to_string(opts.ts_t) + " OR (ts_t = " +
                             std::to_string(opts.ts_t) + " AND ts_i < " +
                             std::to_string(opts.ts_i) + ")");
    case ChangeStreamStart::kHead:
      break;
  }
  return oplog_head(sql);
}

uint64_t oplog_min_seq(SqlSession& sql) {
  return scalar(sql, "SELECT COALESCE(MIN(seq), 0) FROM chimera_meta.oplog");
}

void require_change_stream_history(SqlSession& sql, uint64_t after_seq) {
  // Resuming from the head of an empty oplog is the ordinary cold start.
  if (after_seq == 0) return;

  const uint64_t oldest = oplog_min_seq(sql);
  // An oplog that has never held a row cannot have issued the token being
  // presented, so the token is as lost as a pruned one.
  if (oldest != 0 && after_seq + 1 >= oldest) return;

  throw change_stream_history_lost(
      "the resume point is no longer in the oplog; resume from a later point or resync "
      "(see changestream-plan.md)");
}

OperationTime current_operation_time(SqlSession& sql) {
  OperationTime now;
  ResultSet rows = sql.query("SELECT ts_t, ts_i FROM chimera_meta.oplog_clock WHERE id = 1");
  if (rows.rows.empty() || !rows.rows[0][0] || !rows.rows[0][1]) return now;
  now.t = static_cast<uint32_t>(std::strtoul(rows.rows[0][0]->c_str(), nullptr, 10));
  now.i = static_cast<uint32_t>(std::strtoul(rows.rows[0][1]->c_str(), nullptr, 10));
  return now;
}

}  // namespace chimera
