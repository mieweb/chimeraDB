#include "auth.h"
#include "chimera/error.h"
#include <algorithm>
#include <openssl/hmac.h>
#include <chrono>
#include <thread>

namespace chimera {
namespace {
[[noreturn]] void failed() {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  throw TranslatorError(18, "AuthenticationFailed", "Authentication failed.");
}
std::string text(const bson_t* doc, const char* name) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, doc, name) || !BSON_ITER_HOLDS_UTF8(&it)) failed();
  uint32_t n = 0;
  const char* s = bson_iter_utf8(&it, &n);
  return std::string(s, n);
}
std::string payload(const bson_t* doc) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, doc, "payload") || !BSON_ITER_HOLDS_BINARY(&it)) failed();
  bson_subtype_t subtype;
  uint32_t length;
  const uint8_t* data;
  bson_iter_binary(&it, &subtype, &length, &data);
  if (subtype != BSON_SUBTYPE_BINARY || length > 4096) failed();
  return std::string(reinterpret_cast<const char*>(data), length);
}
std::string dummy_key(const std::string& db, const std::string& user, const char* purpose) {
  static const std::string secret = random_bytes(32);
  const std::string message = hex_encode(db) + ":" + hex_encode(user) + ":" + purpose;
  std::string out(32, '\0');
  unsigned length = 0;
  if (!HMAC(EVP_sha256(), secret.data(), secret.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size(),
            reinterpret_cast<unsigned char*>(out.data()), &length) || length != 32) failed();
  return out;
}
Bson reply(int32_t id, const std::string& bytes, bool done) {
  Bson out;
  BSON_APPEND_INT32(out.get(), "conversationId", id);
  BSON_APPEND_BOOL(out.get(), "done", done);
  BSON_APPEND_BINARY(out.get(), "payload", BSON_SUBTYPE_BINARY,
                     reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  BSON_APPEND_DOUBLE(out.get(), "ok", 1);
  return out;
}
std::string literal(const std::string& value) { return "CONVERT(X'" + hex_encode(value) + "' USING utf8mb4)"; }
}  // namespace

std::optional<ScramCredential> find_credential(SqlSession& sql, const std::string& db,
                                              const std::string& user) {
  if (db.empty() || db.size() > 64 || user.empty() || user.size() > 128 ||
      db.find('\0') != std::string::npos || user.find('\0') != std::string::npos) return {};
  ResultSet result;
  try {
    result = sql.query(
        "SELECT c.sql_host, HEX(c.salt), c.iterations, HEX(c.stored_key), HEX(c.server_key),"
        " HEX(c.revision), COALESCE(JSON_VALUE(p.Priv, '$.default_role'), '') FROM chimera_auth.credentials c JOIN mysql.global_priv p"
        " ON BINARY p.User = BINARY c.user_name AND BINARY p.Host = BINARY c.sql_host"
        " WHERE c.auth_db = " + literal(db) + " AND c.user_name = " + literal(user) +
        " AND COALESCE(JSON_VALUE(p.Priv, '$.account_locked'), 0) = 0");
  } catch (const TranslatorError& e) {
    if (e.code() == 26) return {};  // not provisioned yet: fail closed
    throw;
  }
  if (result.rows.size() != 1 || result.rows[0].size() != 7) return {};
  const auto& row = result.rows[0];
  for (const auto& cell : row) if (!cell) return {};
  ScramCredential out;
  out.auth_db = db; out.user = user; out.sql_host = *row[0];
  out.salt = hex_decode(*row[1]);
  out.iterations = std::stoi(*row[2]);
  const std::string stored = hex_decode(*row[3]), server = hex_decode(*row[4]);
  if (stored.size() != 32 || server.size() != 32 || out.salt.size() < 16 ||
      out.iterations < 4096 || out.iterations > 1000000) return {};
  std::copy(stored.begin(), stored.end(), out.stored_key.begin());
  std::copy(server.begin(), server.end(), out.server_key.begin());
  out.revision = *row[5];
  out.sql_role = *row[6];
  return out;
}
void Authentication::reset() {
  authenticated_ = awaiting_empty_ = false;
  conversation_id_ = 0;
  conversation_.reset();
  credential_ = ScramCredential{};
}
std::string Authentication::principal() const {
  return hex_encode(credential_.auth_db) + ":" + hex_encode(credential_.user) + ":" +
         hex_encode(credential_.sql_host) + ":" + credential_.revision;
}
Bson Authentication::start(const bson_t* body, const std::string& db, SqlSession& sql) {
  reset();
  try {
    if (text(body, "mechanism") != "SCRAM-SHA-256")
      throw TranslatorError(334, "MechanismUnavailable", "Only SCRAM-SHA-256 is supported");
    const std::string bytes = payload(body), user = ScramConversation::username(bytes);
    const auto found = find_credential(sql, db, user);
    if (found) credential_ = *found;
    else {
      credential_.user = user; credential_.auth_db = db;
      // Stable dummy salts prevent repeated starts from identifying unknown
      // users by a changing salt. The process secret never leaves this module.
      credential_.salt = dummy_key(db, user, "salt").substr(0, 16);
      const std::string stored = dummy_key(db, user, "stored"), server = dummy_key(db, user, "server");
      std::copy(stored.begin(), stored.end(), credential_.stored_key.begin());
      std::copy(server.begin(), server.end(), credential_.server_key.begin());
    }
    const std::string challenge = conversation_.start(bytes, credential_, found.has_value());
    const std::string id = random_bytes(4);
    uint32_t n = 0;
    for (unsigned char c : id) n = (n << 8) | c;
    conversation_id_ = static_cast<int32_t>((n & 0x7ffffffe) + 1);
    return reply(conversation_id_, challenge, false);
  } catch (const TranslatorError& e) {
    reset();
    if (e.code() == 334) throw;
    failed();
  } catch (...) { reset(); failed(); }
}
Bson Authentication::more(const bson_t* body, const std::string& db, SqlSession& sql) {
  try {
    bson_iter_t it;
    if (!conversation_id_ || db != credential_.auth_db ||
        !bson_iter_init_find(&it, body, "conversationId") || !BSON_ITER_HOLDS_INT32(&it) ||
        bson_iter_int32(&it) != conversation_id_) failed();
    const std::string bytes = payload(body);
    if (awaiting_empty_) {
      if (!bytes.empty()) failed();
      validate(sql);
      awaiting_empty_ = false;
      return reply(conversation_id_, "", true);
    }
    if (authenticated_) failed();
    const std::string signature = conversation_.finish(bytes);
    authenticated_ = true;
    validate(sql);
    awaiting_empty_ = true;
    // The standard three-exchange form works with both legacy and modern drivers.
    return reply(conversation_id_, signature, false);
  } catch (...) { reset(); failed(); }
}
void Authentication::validate(SqlSession& sql) {
  if (!authenticated_) throw unauthorized("Authentication required");
  const auto current = find_credential(sql, credential_.auth_db, credential_.user);
  if (!current || current->revision != credential_.revision ||
      current->sql_host != credential_.sql_host || current->salt != credential_.salt ||
      current->iterations != credential_.iterations || current->stored_key != credential_.stored_key ||
      current->server_key != credential_.server_key) {
    reset();
    throw unauthorized("Credentials have been revoked; reconnect and authenticate");
  }
  credential_.sql_role = current->sql_role;
}
}  // namespace chimera
