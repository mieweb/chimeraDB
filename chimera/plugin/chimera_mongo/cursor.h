#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "chimera/bson.h"

namespace chimera {

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
  };

  void expire_idle();

  std::mutex mutex_;
  std::unordered_map<int64_t, Entry> cursors_;
  int64_t next_id_ = 1;
};

}  // namespace chimera
