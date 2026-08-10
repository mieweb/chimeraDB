#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "commands.h"

namespace chimera {

// Owns the listening socket and one thread per client connection. Nothing in
// here touches server internals, which is why it can be started and stopped
// from plugin init/deinit without any locking against the server.
class Listener {
public:
  Listener(std::string bind_address, uint16_t port);
  ~Listener();

  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Binds and starts accepting. On failure returns false and fills `error`.
  bool start(std::string* error);

  // Idempotent. Returns only once every thread has been joined, so the server
  // can shut down without leaking threads.
  void stop();

private:
  const std::string bind_address_;
  const uint16_t port_;
  ServerIdentity identity_;

  int listen_fd_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> next_connection_id_{1};

  std::thread accept_thread_;
  std::mutex connections_mutex_;
  std::set<int> live_fds_;
  std::vector<std::thread> connection_threads_;

  void accept_loop();
  void serve(int fd, int64_t connection_id);
  void forget(int fd);
};

}  // namespace chimera
