#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sql.h"
#include "wire.h"

namespace chimera {

// How this server describes itself to clients. Fixed for the life of the process.
struct ServerIdentity {
  std::string host;       // "127.0.0.1:27018" — what hello advertises
  bson_oid_t process_id;  // topologyVersion.processId
};

// Per-connection state. The SQL session is created on first use, not on
// connect: drivers open extra connections purely to monitor the topology, and
// those only ever send hello/ping — giving each one a server THD would be waste.
class ConnectionState {
public:
  ConnectionState(const ServerIdentity& identity, int64_t connection_id);

  SqlSession& sql();
  const ServerIdentity& identity() const { return identity_; }
  int64_t connection_id() const { return connection_id_; }

private:
  const ServerIdentity& identity_;
  int64_t connection_id_;
  std::unique_ptr<SqlSession> sql_;
};

// Builds the reply document for one request. Never throws: a bad command
// becomes a {ok:0, code, codeName, errmsg} envelope, which is what drivers
// expect and what keeps the connection usable.
Bson dispatch_command(const wire::Request& req, ConnectionState& state) noexcept;

}  // namespace chimera
