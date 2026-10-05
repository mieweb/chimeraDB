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
    if (!row[0] || !row[1]) {
      throw internal_error("oplog row rendered NULL — kEventExpr regression");
    }
    batch.last_seq = std::strtoull(row[0]->c_str(), nullptr, 10);
    batch.documents.push_back(from_extjson(*row[1]));
  }
  // Check after every SELECT, including reads after a park. A prune before or
  // during the read must be noticed before callers deliver events or advance
  // their cursor. A prune after this check cannot remove the batch in memory;
  // racing pruning may conservatively require resync, but cannot hide a gap.
  require_change_stream_history(sql, after_seq);
  return batch;
}

uint64_t resolve_change_stream_start(SqlSession& sql, const ChangeStreamOptions& opts) {
  switch (opts.start) {
    case ChangeStreamStart::kToken:
      return opts.after_seq;
    case ChangeStreamStart::kOperationTime: {
      // Read the predecessor and retained edge in ONE snapshot. At the oldest
      // retained event's exact timestamp, no predecessor remains; zero would
      // incorrectly describe a cursor opened on a never-written oplog. Use
      // oldest.seq - 1 instead, so that event is included and subsequent prune
      // checks still detect losing it before the first getMore.
      ResultSet rows = sql.query(
          "SELECT before_time.after_seq, oldest.seq, oldest.ts_t, oldest.ts_i FROM "
          "(SELECT COALESCE(MAX(seq), 0) AS after_seq FROM chimera_meta.oplog WHERE ts_t < " +
          std::to_string(opts.ts_t) + " OR (ts_t = " + std::to_string(opts.ts_t) +
          " AND ts_i < " + std::to_string(opts.ts_i) + ")) AS before_time LEFT JOIN "
          "(SELECT seq, ts_t, ts_i FROM chimera_meta.oplog ORDER BY seq LIMIT 1) AS oldest "
          "ON 1 = 1");
      if (rows.rows.size() != 1 || rows.rows[0].size() != 4 || !rows.rows[0][0]) {
        throw internal_error("could not resolve the change-stream operation time");
      }
      const Row& row = rows.rows[0];
      if (!row[1] && !row[2] && !row[3]) return 0;  // empty since birth
      if (!row[1] || !row[2] || !row[3]) {
        throw internal_error("the oldest oplog row has a NULL sequence or timestamp");
      }
      const uint64_t before = std::strtoull(row[0]->c_str(), nullptr, 10);
      const uint64_t oldest = std::strtoull(row[1]->c_str(), nullptr, 10);
      const uint32_t oldest_t = static_cast<uint32_t>(std::strtoul(row[2]->c_str(), nullptr, 10));
      const uint32_t oldest_i = static_cast<uint32_t>(std::strtoul(row[3]->c_str(), nullptr, 10));
      const bool before_retained_time = opts.ts_t < oldest_t ||
                                       (opts.ts_t == oldest_t && opts.ts_i < oldest_i);
      // If row 1 survives, all history survives: an earlier start is safe.
      // Otherwise a time older than the retained edge may have lost events.
      if (oldest > 1 && before_retained_time) {
        throw change_stream_history_lost(
            "the requested operation time is older than the retained oplog; resync "
            "(see changestream-plan.md)");
      }
      return before != 0 ? before : oldest - 1;
    }
    case ChangeStreamStart::kHead:
      break;
  }
  return oplog_head(sql);
}

uint64_t oplog_min_seq(SqlSession& sql) {
  return scalar(sql, "SELECT COALESCE(MIN(seq), 0) FROM chimera_meta.oplog");
}

void require_change_stream_history(SqlSession& sql, uint64_t after_seq) {
  const uint64_t oldest = oplog_min_seq(sql);
  // Zero is a valid position only while the oplog is still empty or starts at
  // its first row. A cursor opened before the first write must still detect a
  // later prune; bypassing every check for zero would silently lose that burst.
  if (oldest == 0 && after_seq == 0) return;
  // An oplog that has never held a row cannot have issued the token being
  // presented, so the token is as lost as a pruned one.
  // Subtract from the nonzero minimum rather than overflowing after_seq + 1.
  if (oldest != 0 && after_seq >= oldest - 1) return;

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

OperationTime current_operation_time_or_initialize(SqlSession& sql) {
  try {
    return current_operation_time(sql);
  } catch (const TranslatorError& error) {
    // The first ping or no-op delete may precede both the first write and the
    // pruner's initial pass. Only the SQL adapter's missing-table/database
    // error justifies bootstrapping; permission/storage failures stay visible.
    if (error.code() != 26) throw;
  }
  // A caller can have an explicit transaction open through chimeraSql even
  // between Mongo commands. DDL must use its own session so first-ping setup
  // cannot implicitly commit that caller's work.
  SqlSession initializer;
  install_oplog_schema(initializer);
  // A failed install or retry propagates. Do not manufacture a zero timestamp
  // or loop indefinitely when the underlying schema cannot be made usable.
  return current_operation_time(sql);
}

}  // namespace chimera
