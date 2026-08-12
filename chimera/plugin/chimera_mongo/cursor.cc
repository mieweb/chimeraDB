#include "cursor.h"

#include <ctime>

#include "chimera/error.h"

namespace chimera {
namespace {

// MongoDB's own default. A driver that walks away mid-iteration must not pin
// documents in this process forever.
constexpr int64_t kIdleTimeoutSeconds = 600;

int64_t now_seconds() { return static_cast<int64_t>(::time(nullptr)); }

}  // namespace

CursorRegistry& CursorRegistry::instance() {
  static CursorRegistry registry;
  return registry;
}

void CursorRegistry::expire_idle() {
  const int64_t cutoff = now_seconds() - kIdleTimeoutSeconds;
  for (auto it = cursors_.begin(); it != cursors_.end();) {
    it = it->second.last_used_seconds < cutoff ? cursors_.erase(it) : std::next(it);
  }
}

int64_t CursorRegistry::open(std::string ns, std::vector<Bson> remaining) {
  if (remaining.empty()) return 0;
  std::lock_guard<std::mutex> guard(mutex_);
  expire_idle();
  const int64_t id = next_id_++;
  Entry entry;
  entry.ns = std::move(ns);
  entry.remaining = std::move(remaining);
  entry.last_used_seconds = now_seconds();
  cursors_.emplace(id, std::move(entry));
  return id;
}

int64_t CursorRegistry::open_tail(std::string ns, TailState state) {
  std::lock_guard<std::mutex> guard(mutex_);
  expire_idle();
  const int64_t id = next_id_++;
  Entry entry;
  entry.ns = std::move(ns);
  entry.last_used_seconds = now_seconds();
  entry.tailing = true;
  entry.tail = std::move(state);
  cursors_.emplace(id, std::move(entry));
  return id;
}

bool CursorRegistry::tail_state(int64_t id, const std::string& ns, TailState* state) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = cursors_.find(id);
  if (it == cursors_.end() || it->second.ns != ns || !it->second.tailing) return false;
  it->second.last_used_seconds = now_seconds();
  *state = it->second.tail.clone();
  return true;
}

void CursorRegistry::advance_tail(int64_t id, uint64_t after_seq) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = cursors_.find(id);
  if (it != cursors_.end()) it->second.tail.after_seq = after_seq;
}

std::vector<Bson> CursorRegistry::next_batch(int64_t id, const std::string& ns,
                                             int32_t batch_size, bool* exhausted) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = cursors_.find(id);
  if (it == cursors_.end()) {
    throw cursor_not_found("cursor id " + std::to_string(id) + " not found");
  }
  // A cursor belongs to the namespace it was opened on; honouring that keeps a
  // mismatched getMore from silently returning another collection's documents.
  if (it->second.ns != ns) {
    throw cursor_not_found("cursor id " + std::to_string(id) + " is not on " + ns);
  }

  Entry& entry = it->second;
  entry.last_used_seconds = now_seconds();
  const size_t wanted = batch_size > 0 ? static_cast<size_t>(batch_size) : entry.remaining.size();
  std::vector<Bson> batch;
  while (batch.size() < wanted && entry.offset < entry.remaining.size()) {
    batch.push_back(std::move(entry.remaining[entry.offset++]));
  }

  *exhausted = entry.offset >= entry.remaining.size();
  if (*exhausted) cursors_.erase(it);
  return batch;
}

bool CursorRegistry::kill(int64_t id) {
  std::lock_guard<std::mutex> guard(mutex_);
  return cursors_.erase(id) > 0;
}

void CursorRegistry::kill_all() {
  std::lock_guard<std::mutex> guard(mutex_);
  cursors_.clear();
}

}  // namespace chimera
