#pragma once

#include <cstdint>
#include <string>

#include "wire.h"

namespace chimera {

// How this server describes itself to clients. Fixed for the life of the process.
struct ServerIdentity {
  std::string host;       // "127.0.0.1:27018" — what hello advertises
  bson_oid_t process_id;  // topologyVersion.processId
};

// Builds the reply document for one request. Never throws: a bad command
// becomes a {ok:0, code, codeName, errmsg} envelope, which is what drivers
// expect and what keeps the connection usable.
Bson dispatch_command(const wire::Request& req, const ServerIdentity& identity,
                      int64_t connection_id) noexcept;

}  // namespace chimera
