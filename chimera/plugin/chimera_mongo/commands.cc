#include "commands.h"

#include <sys/time.h>

#include "chimera/error.h"

namespace chimera {
namespace {

constexpr int32_t kMaxBsonObjectSize = 16 * 1024 * 1024;

// Wire version 17 is MongoDB 6.0. It is the lowest version that advertises
// everything Meteor's driver requires — logical sessions, retryable writes and
// the `hello` command — without promising 7.0/8.0 features we do not implement.
constexpr int32_t kMaxWireVersion = 17;
constexpr int32_t kMinWireVersion = 0;

int64_t now_millis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

Bson ok_reply() {
  Bson reply;
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson error_reply(int code, const char* code_name, const std::string& message) {
  Bson reply;
  bson_t* b = reply.get();
  BSON_APPEND_DOUBLE(b, "ok", 0);
  BSON_APPEND_UTF8(b, "errmsg", message.c_str());
  BSON_APPEND_INT32(b, "code", code);
  BSON_APPEND_UTF8(b, "codeName", code_name);
  return reply;
}

Bson hello_reply(const ServerIdentity& identity, int64_t connection_id) {
  Bson reply;
  bson_t* b = reply.get();
  BSON_APPEND_BOOL(b, "helloOk", true);
  // `ismaster` is the legacy spelling; drivers still handshaking over OP_QUERY
  // read that one, newer ones read `isWritablePrimary`. Send both.
  BSON_APPEND_BOOL(b, "ismaster", true);
  BSON_APPEND_BOOL(b, "isWritablePrimary", true);

  // Meteor tails an oplog, and a driver only looks for one on a replica set.
  // ChimeraDB therefore presents itself as a single-node set (D7).
  BSON_APPEND_UTF8(b, "setName", "chimera");
  BSON_APPEND_INT32(b, "setVersion", 1);
  BSON_APPEND_BOOL(b, "secondary", false);

  bson_t hosts;
  BSON_APPEND_ARRAY_BEGIN(b, "hosts", &hosts);
  BSON_APPEND_UTF8(&hosts, "0", identity.host.c_str());
  bson_append_array_end(b, &hosts);
  BSON_APPEND_UTF8(b, "primary", identity.host.c_str());
  BSON_APPEND_UTF8(b, "me", identity.host.c_str());

  bson_t topology;
  BSON_APPEND_DOCUMENT_BEGIN(b, "topologyVersion", &topology);
  BSON_APPEND_OID(&topology, "processId", &identity.process_id);
  BSON_APPEND_INT64(&topology, "counter", 0);
  bson_append_document_end(b, &topology);

  BSON_APPEND_INT32(b, "maxBsonObjectSize", kMaxBsonObjectSize);
  BSON_APPEND_INT32(b, "maxMessageSizeBytes", wire::kMaxMessageBytes);
  BSON_APPEND_INT32(b, "maxWriteBatchSize", 100000);
  BSON_APPEND_DATE_TIME(b, "localTime", now_millis());
  BSON_APPEND_INT32(b, "logicalSessionTimeoutMinutes", 30);
  BSON_APPEND_INT32(b, "connectionId", static_cast<int32_t>(connection_id));
  BSON_APPEND_INT32(b, "minWireVersion", kMinWireVersion);
  BSON_APPEND_INT32(b, "maxWireVersion", kMaxWireVersion);
  BSON_APPEND_BOOL(b, "readOnly", false);
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

Bson build_info_reply() {
  Bson reply;
  bson_t* b = reply.get();
  // The version string is what drivers gate their feature checks on, so it has
  // to match the wire version we advertise.
  BSON_APPEND_UTF8(b, "version", "6.0.0");
  bson_t array;
  BSON_APPEND_ARRAY_BEGIN(b, "versionArray", &array);
  BSON_APPEND_INT32(&array, "0", 6);
  BSON_APPEND_INT32(&array, "1", 0);
  BSON_APPEND_INT32(&array, "2", 0);
  BSON_APPEND_INT32(&array, "3", 0);
  bson_append_array_end(b, &array);
  BSON_APPEND_UTF8(b, "gitVersion", "chimera");
  BSON_APPEND_UTF8(b, "sysInfo", "ChimeraDB on MariaDB");
  BSON_APPEND_INT32(b, "bits", 64);
  BSON_APPEND_BOOL(b, "debug", false);
  BSON_APPEND_INT32(b, "maxBsonObjectSize", kMaxBsonObjectSize);
  bson_t modules;
  BSON_APPEND_ARRAY_BEGIN(b, "modules", &modules);
  bson_append_array_end(b, &modules);
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

}  // namespace

Bson dispatch_command(const wire::Request& req, const ServerIdentity& identity,
                      int64_t connection_id) noexcept {
  try {
    auto fields = document_fields(req.body.get());
    if (fields.empty()) return error_reply(9, "FailedToParse", "command document is empty");

    // MongoDB identifies a command by the *first* field of the document.
    const std::string& command = fields.front().first;

    if (command == "hello" || command == "isMaster" || command == "ismaster") {
      return hello_reply(identity, connection_id);
    }
    if (command == "ping") return ok_reply();
    if (command == "buildInfo" || command == "buildinfo") return build_info_reply();
    if (command == "endSessions") return ok_reply();  // sessions are accepted and ignored

    return error_reply(59, "CommandNotFound", "no such command: '" + command + "'");
  } catch (const TranslatorError& e) {
    return error_reply(e.code(), e.code_name().c_str(), e.what());
  } catch (const std::exception& e) {
    return error_reply(1, "InternalError", e.what());
  } catch (...) {
    return error_reply(1, "InternalError", "unknown failure while handling command");
  }
}

}  // namespace chimera
