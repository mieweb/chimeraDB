#pragma once

#include <cstdint>

#include "chimera/changestream.h"
#include "collection.h"
#include "oplog.h"
#include "sql.h"

namespace chimera {

// Change streams served from the M5 oplog. A change stream *is* a tailing oplog
// cursor wearing a different document shape, so everything here is the reskin:
// the same table, the same total order, the same park-on-commit machinery, one
// SQL expression away from a different document.

// A page of change events for one collection, oldest first — a change stream has
// no `{$natural: -1}` form. Filtering is `ns` equality and nothing else, which is
// the only filter a change stream can ever need, so `compile_filter` stays out
// of it.
OplogBatch read_changestream(SqlSession& sql, const Namespace& ns, uint64_t after_seq,
                             uint64_t limit);

// Turns the parsed start options into the sequence to read strictly after.
// `startAtOperationTime` is inclusive of events *at* that time, so it resolves to
// the last sequence strictly before it.
uint64_t resolve_change_stream_start(SqlSession& sql, const ChangeStreamOptions& opts);

// The oldest sequence still retained, or 0 when the oplog has never held a row.
uint64_t oplog_min_seq(SqlSession& sql);

// Throws ChangeStreamHistoryLost when `after_seq` names a point the pruner has
// already discarded, so a resuming client is told to resync rather than handed a
// stream with a silent hole in it.
void require_change_stream_history(SqlSession& sql, uint64_t after_seq);

// The current oplog clock, as `operationTime` for a reply. Read from the same
// single-row table the append procedure stamps under its lock.
struct OperationTime {
  uint32_t t = 0;
  uint32_t i = 0;
};
OperationTime current_operation_time(SqlSession& sql);

}  // namespace chimera
