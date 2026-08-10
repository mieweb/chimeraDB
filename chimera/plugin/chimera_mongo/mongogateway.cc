// Mongo for clients that only speak SQL.
//
// A pasted shell statement becomes a command document and goes through
// `dispatch_command` — the same entry point the wire listener uses. That is the
// whole design: the gateway adds a spelling, not a second implementation, so a
// `mongo('db.c.insertOne(…)')` and a driver's insert cannot drift apart, and in
// particular cannot produce different oplog entries.
#include "mongogateway.h"

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/mongosh.h"
#include "commands.h"

namespace chimera {
namespace {

// The gateway is not a network client, but the command handlers describe
// themselves to one, so it needs an identity. It is created once and never
// changes.
const ServerIdentity& gateway_identity() {
  static const ServerIdentity identity = [] {
    ServerIdentity id;
    id.host = "chimera-sql-gateway";
    bson_oid_init(&id.process_id, nullptr);
    return id;
  }();
  return identity;
}

Bson run_command(const Bson& command, const std::string& db, ConnectionState& state) {
  wire::Request request;
  request.body = Bson::copy_of(command.get());
  request.database = db;
  Bson reply = dispatch_command(request, state);

  bson_iter_t it;
  if (bson_iter_init_find(&it, reply.get(), "ok") && value_as_double(*bson_iter_value(&it)) == 1) {
    return reply;
  }
  std::string message = "the command failed";
  if (bson_iter_init_find(&it, reply.get(), "errmsg") && BSON_ITER_HOLDS_UTF8(&it)) {
    message = bson_iter_utf8(&it, nullptr);
  }
  int code = 1;
  std::string code_name = "InternalError";
  if (bson_iter_init_find(&it, reply.get(), "code")) code = bson_iter_int32(&it);
  if (bson_iter_init_find(&it, reply.get(), "codeName") && BSON_ITER_HOLDS_UTF8(&it)) {
    code_name = bson_iter_utf8(&it, nullptr);
  }
  throw TranslatorError(code, code_name, message);
}

bool view_field(const bson_t* doc, const char* name, bson_t* out) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, doc, name)) return false;
  const uint8_t* data = nullptr;
  uint32_t length = 0;
  if (BSON_ITER_HOLDS_DOCUMENT(&it)) {
    bson_iter_document(&it, &length, &data);
  } else if (BSON_ITER_HOLDS_ARRAY(&it)) {
    bson_iter_array(&it, &length, &data);
  } else {
    return false;
  }
  return bson_init_static(out, data, length);
}

// Drains a cursor rather than returning its first batch. A SQL caller has no
// way to ask for the rest, so stopping at 101 documents would be a silent
// truncation — the one failure mode a gateway must not have.
std::vector<Bson> collect_cursor(Bson reply, const std::string& db, ConnectionState& state) {
  std::vector<Bson> documents;
  const char* batch_name = "firstBatch";
  while (true) {
    bson_t cursor;
    if (!view_field(reply.get(), "cursor", &cursor)) break;
    bson_t batch;
    if (view_field(&cursor, batch_name, &batch)) {
      for (const auto& [key, element] : document_fields(&batch)) {
        (void)key;
        bson_t document;
        bson_init_static(&document, element.get().value.v_doc.data,
                         element.get().value.v_doc.data_len);
        documents.push_back(Bson::copy_of(&document));
      }
    }

    bson_iter_t it;
    if (!bson_iter_init_find(&it, &cursor, "id") || bson_iter_int64(&it) == 0) break;
    const int64_t cursor_id = bson_iter_int64(&it);
    std::string ns;
    if (bson_iter_init_find(&it, &cursor, "ns") && BSON_ITER_HOLDS_UTF8(&it)) {
      ns = bson_iter_utf8(&it, nullptr);
    }
    const size_t dot = ns.find('.');

    Bson more;
    BSON_APPEND_INT64(more.get(), "getMore", cursor_id);
    BSON_APPEND_UTF8(more.get(), "collection",
                     dot == std::string::npos ? ns.c_str() : ns.c_str() + dot + 1);
    reply = run_command(more, db, state);
    batch_name = "nextBatch";
  }
  return documents;
}

std::string render_array(const std::vector<Bson>& documents) {
  std::string text = "[";
  for (size_t i = 0; i < documents.size(); ++i) {
    if (i != 0) text += ", ";
    text += to_extjson(documents[i].get());
  }
  return text + "]";
}

}  // namespace

std::string run_mongo_gateway(const std::string& db, const std::string& statement) {
  if (db.empty()) {
    throw bad_value(
        "mongo() needs a database: USE one first, or call mongo('<database>', '<statement>')");
  }
  const ShellCommand command = build_shell_command(parse_shell_call(statement));

  ConnectionState state(gateway_identity(), 0);
  Bson reply = run_command(command.command, db, state);

  switch (command.result) {
    case ShellResult::Documents:
      return render_array(collect_cursor(std::move(reply), db, state));
    case ShellResult::FirstDocument: {
      const std::vector<Bson> documents = collect_cursor(std::move(reply), db, state);
      return documents.empty() ? "null" : to_extjson(documents.front().get());
    }
    case ShellResult::Count: {
      bson_iter_t it;
      if (!bson_iter_init_find(&it, reply.get(), "n")) return "0";
      return std::to_string(static_cast<int64_t>(value_as_double(*bson_iter_value(&it))));
    }
    case ShellResult::Reply:
      break;
  }
  return to_extjson(reply.get());
}

}  // namespace chimera
