#include "listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "chimera/error.h"

namespace chimera {
namespace {

// How long accept() waits before re-checking the shutdown flag. Polling keeps
// shutdown free of the fd-reuse race that closing a socket out from under a
// blocked accept() would introduce, and 200 ms is invisible at shutdown.
constexpr int kAcceptPollMs = 200;

std::string errno_message(const char* what) {
  return std::string(what) + ": " + strerror(errno);
}

}  // namespace

Listener::Listener(std::string bind_address, uint16_t port)
    : bind_address_(std::move(bind_address)), port_(port) {
  identity_.host = bind_address_ + ":" + std::to_string(port_);
  bson_oid_init(&identity_.process_id, nullptr);
}

Listener::~Listener() { stop(); }

bool Listener::start(std::string* error) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    *error = errno_message("socket");
    return false;
  }

  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) != 1) {
    *error = "chimera_mongo_bind is not a valid IPv4 address: " + bind_address_;
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
    *error = errno_message("bind");
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 128) != 0) {
    *error = errno_message("listen");
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  accept_thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void Listener::stop() {
  if (stopping_.exchange(true)) return;

  if (accept_thread_.joinable()) accept_thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  // Unblock every connection thread parked in recv(), then wait for them.
  {
    std::lock_guard<std::mutex> guard(connections_mutex_);
    for (int fd : live_fds_) ::shutdown(fd, SHUT_RDWR);
  }
  for (auto& thread : connection_threads_) {
    if (thread.joinable()) thread.join();
  }
  connection_threads_.clear();
}

void Listener::accept_loop() {
  while (!stopping_.load()) {
    struct pollfd waiting = {listen_fd_, POLLIN, 0};
    int ready = ::poll(&waiting, 1, kAcceptPollMs);
    if (ready <= 0) {
      if (ready < 0 && errno != EINTR) break;
      continue;
    }

    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      break;
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    int64_t connection_id = next_connection_id_++;
    std::lock_guard<std::mutex> guard(connections_mutex_);
    if (stopping_.load()) {
      ::close(fd);
      break;
    }
    live_fds_.insert(fd);
    connection_threads_.emplace_back([this, fd, connection_id] { serve(fd, connection_id); });
  }
}

void Listener::serve(int fd, int64_t connection_id) {
  std::vector<uint8_t> raw;
  while (!stopping_.load()) {
    bool alive;
    try {
      alive = wire::read_message(fd, raw);
    } catch (const std::exception&) {
      break;  // a frame we cannot even measure — the stream is unusable
    }
    if (!alive) break;

    wire::Request request;
    Bson reply;
    try {
      request.header = wire::parse_header(raw);
      // A legacy handshake must still be answered as OP_REPLY even if its body
      // turns out to be junk, or the client will hang waiting for a frame it
      // can decode.
      request.legacy_query = request.header.op_code == wire::kOpQuery;
      request = wire::parse_request(raw);
      reply = dispatch_command(request, identity_, connection_id);
    } catch (const TranslatorError& e) {
      reply = Bson();
      BSON_APPEND_DOUBLE(reply.get(), "ok", 0);
      BSON_APPEND_UTF8(reply.get(), "errmsg", e.what());
      BSON_APPEND_INT32(reply.get(), "code", e.code());
      BSON_APPEND_UTF8(reply.get(), "codeName", e.code_name().c_str());
    } catch (const std::exception&) {
      break;
    }

    if (!wire::write_all(fd, wire::encode_reply(request, static_cast<int32_t>(connection_id),
                                                reply.get()))) {
      break;
    }
  }

  forget(fd);
  ::close(fd);
}

void Listener::forget(int fd) {
  std::lock_guard<std::mutex> guard(connections_mutex_);
  live_fds_.erase(fd);
}

}  // namespace chimera
