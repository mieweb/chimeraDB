#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chimera/bson.h"
#include "collection.h"
#include "sql.h"

namespace chimera {

// Meteor tails `local.oplog.rs`, so that is the namespace we answer on. Nothing
// called `local` is ever created as a real database.
extern const char kOplogDb[];
extern const char kOplogCollection[];

// True for the one namespace served from chimera_meta.oplog rather than from a
// collection table.
bool is_oplog_namespace(const Namespace& ns);

// Creates the oplog table, its clock, and the procedure the per-collection
// triggers call. Idempotent.
void install_oplog_schema(SqlSession& sql);

// Mirrors one collection table into the oplog. Triggers are the *only* writer,
// so a write over the wire and a write from the `mariadb` client produce
// identical entries. DROP TABLE takes them with it, so there is no uninstall.
void install_oplog_triggers(SqlSession& sql, const Namespace& ns);

// A page of oplog entries as Mongo documents — {ts, op, ns, o, o2, v, wall} —
// in commit order. `filter` is compiled against that shape, so a client's
// `{ts: {$gt: …}}` behaves as it does on a replica set.
struct OplogBatch {
  std::vector<Bson> documents;
  uint64_t last_seq = 0;  // where a tailing cursor resumes
};
// `newest_first` answers the `{$natural: -1}` shape a client uses to ask "what
// is the head right now?" before it starts tailing.
OplogBatch read_oplog(SqlSession& sql, const bson_t* filter, uint64_t after_seq, uint64_t limit,
                      bool newest_first);

// The highest sequence currently stored. A tail that wants "only what happens
// next" starts here.
uint64_t oplog_head(SqlSession& sql);

// Capped-collection emulation: trims by age and by row count, whichever bites
// first; either limit is off when zero. Returns the number of rows removed.
uint64_t prune_oplog(SqlSession& sql, uint64_t max_rows, uint64_t max_age_seconds);

// Parks until a write is signalled or `timeout_ms` elapses. Returns true when a
// write arrived, which is what lets `awaitData` block instead of spin.
bool wait_for_oplog_write(uint64_t timeout_ms);

// Wakes every parked cursor. Called once a wire write has committed.
void signal_oplog_write();

// What makes the oplog behave like a capped collection: a background thread
// trims it on a fixed cadence. Both limits are re-read on every pass, so `SET
// GLOBAL` takes effect without a restart, and either may be zero to switch that
// half of the policy off.
class OplogPruner {
public:
  OplogPruner(const unsigned long long* max_rows, const unsigned long long* max_age_seconds);
  ~OplogPruner();
  OplogPruner(const OplogPruner&) = delete;
  OplogPruner& operator=(const OplogPruner&) = delete;

private:
  void run();

  const unsigned long long* max_rows_;
  const unsigned long long* max_age_seconds_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace chimera
