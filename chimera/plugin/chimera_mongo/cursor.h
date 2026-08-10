#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "chimera/bson.h"

namespace chimera {

// Where a tailing cursor has read up to, and how it should behave when it
// catches up. Tailing cursors buffer nothing: each getMore re-reads the oplog,
// so the registry never holds a SQL session or a blocked thread.
struct TailState {
  Bson filter;
  uint64_t after_seq = 0;
  bool await_data = false;
};

// Cursors are materialized: a find runs to completion and the remaining
// documents wait here for getMore. That trades memory for exact BSON ordering
// semantics, which MariaDB's JSON functions cannot express (see M8's
// compile-to-SQL fast path).
class CursorRegistry {
public:
  static CursorRegistry& instance();

  // Returns 0 when nothing is left over, which is how a driver knows the cursor
  // is already exhausted.
  int64_t open(std::string ns, std::vector<Bson> remaining);

  // A tailing cursor never reports itself exhausted, so the id is always real.
  int64_t open_tail(std::string ns, TailState state);

  // Fills `state` and returns true when the cursor tails; the caller does the
  // reading, because only it holds a SQL session.
  bool tail_state(int64_t id, const std::string& ns, TailState* state);
  void advance_tail(int64_t id, uint64_t after_seq);

  // Throws CursorNotFound if the id is unknown or belongs to another namespace.
  std::vector<Bson> next_batch(int64_t id, const std::string& ns, int32_t batch_size,
                               bool* exhausted);

  bool kill(int64_t id);
  void kill_all();

private:
  struct Entry {
    std::string ns;
    std::vector<Bson> remaining;
    size_t offset = 0;
    int64_t last_used_seconds = 0;
    bool tailing = false;
    TailState tail;
  };

  void expire_idle();

  std::mutex mutex_;
  std::unordered_map<int64_t, Entry> cursors_;
  int64_t next_id_ = 1;
};

}  // namespace chimera
