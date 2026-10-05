#include "changestream.h"

#include <algorithm>
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
      // Read the predecessor and durable deletion boundary in one snapshot.
      // Sequence gaps alone mean nothing: rollbacks consume AUTO_INCREMENTs.
      ResultSet rows = sql.query(
          "SELECT before_time.after_seq, h.pruned_seq, h.pruned_ts_t, h.pruned_ts_i, "
          "h.legacy_baseline, h.baseline_seq, h.baseline_ts_t, h.baseline_ts_i FROM "
          "(SELECT COALESCE(MAX(seq), 0) AS after_seq FROM chimera_meta.oplog WHERE ts_t < " +
          std::to_string(opts.ts_t) + " OR (ts_t = " + std::to_string(opts.ts_t) +
          " AND ts_i < " + std::to_string(opts.ts_i) + ")) AS before_time "
          "JOIN chimera_meta.oplog_history AS h ON h.id = 1");
      if (rows.rows.size() != 1 || rows.rows[0].size() != 8 ||
          std::any_of(rows.rows[0].begin(), rows.rows[0].end(),
                      [](const auto& value) { return !value; })) {
        throw internal_error("could not resolve the change-stream operation time");
      }
      const Row& row = rows.rows[0];
      const uint64_t before = std::strtoull(row[0]->c_str(), nullptr, 10);
      const uint64_t pruned = std::strtoull(row[1]->c_str(), nullptr, 10);
      const uint32_t pruned_t = static_cast<uint32_t>(std::strtoul(row[2]->c_str(), nullptr, 10));
      const uint32_t pruned_i = static_cast<uint32_t>(std::strtoul(row[3]->c_str(), nullptr, 10));
      const bool legacy = *row[4] != "0";
      const uint64_t baseline = std::strtoull(row[5]->c_str(), nullptr, 10);
      const uint32_t baseline_t = static_cast<uint32_t>(std::strtoul(row[6]->c_str(), nullptr, 10));
      const uint32_t baseline_i = static_cast<uint32_t>(std::strtoul(row[7]->c_str(), nullptr, 10));
      // Time origins are inclusive, so equality with a deleted event is lost.
      // A legacy boundary names a retained event instead: equality is safe.
      const bool deleted = pruned != 0 && (opts.ts_t < pruned_t ||
                           (opts.ts_t == pruned_t && opts.ts_i <= pruned_i));
      const bool unknown = legacy && (opts.ts_t < baseline_t ||
                           (opts.ts_t == baseline_t && opts.ts_i < baseline_i));
      if (deleted || unknown) {
        throw change_stream_history_lost(
            "the requested operation time is older than the retained oplog; resync "
            "(see changestream-plan.md)");
      }
      // A safe time may lie between the last deleted event and the oldest
      // survivor. Its retained predecessor is zero, but resuming after the
      // deletion watermark includes all events at or after the requested time.
      return std::max({before, pruned, baseline});
    }
    case ChangeStreamStart::kHead:
      break;
  }
  return oplog_head(sql);
}

void require_change_stream_history(SqlSession& sql, uint64_t after_seq) {
  const ResultSet rows = sql.query(
      "SELECT GREATEST(pruned_seq, baseline_seq), "
      "EXISTS(SELECT 1 FROM chimera_meta.oplog LIMIT 1) "
      "FROM chimera_meta.oplog_history WHERE id = 1");
  if (rows.rows.size() != 1 || rows.rows[0].size() != 2 ||
      !rows.rows[0][0] || !rows.rows[0][1]) {
    throw internal_error("oplog history metadata is missing");
  }
  const uint64_t floor = std::strtoull(rows.rows[0][0]->c_str(), nullptr, 10);
  // Resume tokens are exclusive: after the last deleted event is still safe.
  // A never-written oplog cannot have issued a positive token. This existence
  // check preserves that validation without mistaking sequence gaps for loss.
  if (after_seq >= floor && (after_seq == 0 || *rows.rows[0][1] != "0")) return;

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
  // The installer uses its own DDL session, but must see the actual caller to
  // refuse migration while that caller holds an explicit SQL transaction.
  install_oplog_schema(sql);
  // A failed install or retry propagates. Do not manufacture a zero timestamp
  // or loop indefinitely when the underlying schema cannot be made usable.
  return current_operation_time(sql);
}

}  // namespace chimera
