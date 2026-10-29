// The only translation unit besides chimera_mongo.cc that sees MariaDB headers.
#include <my_global.h>

#define MYSQL_SERVER 1
#include <mysql.h>
#include <mysql_version.h>
#include <sql_class.h>
#include <sql_acl.h>
#include <mdl.h>
#include <protocol.h>
#include <mysql/plugin.h>
#include <mysql/service_sql.h>
#include <my_sys.h>

#include "sql.h"

#include "chimera/error.h"

namespace chimera {
namespace {

constexpr unsigned kErrDupEntry = 1062;
constexpr unsigned kErrNoSuchTable = 1146;
constexpr unsigned kErrTableExists = 1050;
constexpr unsigned kErrBadDb = 1049;

MYSQL* as_mysql(void* handle) { return static_cast<MYSQL*>(handle); }
THD* session_thd(void* handle) {
  // mysql_real_connect_local stores its Protocol_local, whose public Protocol
  // base exposes the executing THD, in MYSQL::thd (on both supported series).
  return static_cast<Protocol*>(as_mysql(handle)->thd)->thd;
}
struct SessionIdentity {
  std::string user, host;
  Security_context context;
};

// Enough of the server's type zoo to answer "is this a number?". Anything not
// listed is handed on as text, which is always a truthful answer even when a
// richer one exists.
ColumnType column_type_of(const MYSQL_FIELD& field) {
  switch (field.type) {
    case MYSQL_TYPE_TINY:
      // TINYINT(1) is how the server spells a boolean, and how chimera's own
      // `is_unique` catalog column is declared.
      return field.length == 1 ? ColumnType::Bool : ColumnType::Integer;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      return ColumnType::Integer;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      return ColumnType::Real;
    default:
      // DECIMAL lands here deliberately: turning it into a double would lose
      // the precision it was chosen for.
      return ColumnType::Text;
  }
}

// Server error numbers carry meaning the mongo client needs; anything we do not
// map deliberately surfaces as InternalError with the server's own text, which
// is more useful than a guessed mongo code.
[[noreturn]] void raise(MYSQL* mysql, const std::string& sql) {
  const unsigned code = mysql_errno(mysql);
  const std::string detail = std::string(mysql_error(mysql)) + " [" + sql.substr(0, 400) + "]";
  switch (code) {
    case 1044: case 1045: case 1142: case 1143: case 1227: case 1356: case 1370:
      throw unauthorized("SQL access denied");
    case kErrDupEntry:
      throw duplicate_key(detail);
    case kErrNoSuchTable:
    case kErrBadDb:
      throw namespace_not_found(detail);
    case kErrTableExists:
      throw namespace_exists(detail);
    default:
      throw internal_error(detail);
  }
}

}  // namespace

SqlThreadScope::SqlThreadScope() { my_thread_init(); }
SqlThreadScope::~SqlThreadScope() { my_thread_end(); }

SqlSession::SqlSession() : SqlSession(false) {}

SqlSession::SqlSession(bool independent) {
  MYSQL* mysql = mysql_init(nullptr);
  if (mysql == nullptr) throw internal_error("mysql_init failed");
  // Restricted sessions must own a THD: the service's borrowed-THD path
  // deliberately substitutes skip_grants. Internal engine sessions retain the
  // original behavior when the service can reuse a SQL mongo() caller's THD.
  THD* previous = current_thd;
  if (independent) set_current_thd(nullptr);
  MYSQL* connected = mysql_real_connect_local(mysql);
  set_current_thd(previous);
  if (connected == nullptr) {
    mysql_close(mysql);
    throw internal_error("could not open a local connection to the server");
  }
  handle_ = mysql;
  // Documents are stored as extJSON text, so the session must speak the same
  // charset the collection tables use or multi-byte content round-trips wrong.
  mysql_set_character_set(mysql, "utf8mb4");
}

SqlSession::~SqlSession() {
  if (handle_ != nullptr) {
    if (identity_) session_thd(handle_)->security_ctx = &session_thd(handle_)->main_security_ctx;
    mysql_close(as_mysql(handle_));
  }
  delete static_cast<SessionIdentity*>(identity_);
}

void SqlSession::exec(const std::string& sql) {
  MYSQL* mysql = as_mysql(handle_);
  if (mysql_real_query(mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    raise(mysql, sql);
  }
  // A statement may still carry a result set (a stray SELECT); drain it so the
  // connection is left usable.
  if (MYSQL_RES* res = mysql_store_result(mysql)) mysql_free_result(res);
}

ResultSet SqlSession::query(const std::string& sql) {
  MYSQL* mysql = as_mysql(handle_);
  if (mysql_real_query(mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    raise(mysql, sql);
  }

  MYSQL_RES* res = mysql_store_result(mysql);
  if (res == nullptr) {
    if (mysql_errno(mysql) != 0) raise(mysql, sql);
    return ResultSet{};
  }

  ResultSet out;
  const unsigned field_count = mysql_num_fields(res);
  MYSQL_FIELD* fields = mysql_fetch_fields(res);
  out.columns.reserve(field_count);
  for (unsigned i = 0; i < field_count; ++i) {
    out.columns.push_back(Column{fields[i].name, column_type_of(fields[i])});
  }

  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    unsigned long* lengths = mysql_fetch_lengths(res);
    Row values;
    values.reserve(field_count);
    for (unsigned i = 0; i < field_count; ++i) {
      if (row[i] == nullptr) {
        values.emplace_back(std::nullopt);
      } else {
        values.emplace_back(std::string(row[i], lengths[i]));
      }
    }
    out.rows.push_back(std::move(values));
  }
  mysql_free_result(res);
  return out;
}

uint64_t SqlSession::affected_rows() const {
  return static_cast<uint64_t>(mysql_affected_rows(as_mysql(handle_)));
}

bool SqlSession::in_transaction() const {
  return (as_mysql(handle_)->server_status & SERVER_STATUS_IN_TRANS) != 0;
}

std::string SqlSession::quote(std::string_view text) const {
  return quote_binary(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string SqlSession::quote_binary(const uint8_t* data, size_t length) const {
  std::string escaped(2 * length + 1, '\0');
  const unsigned long written = mysql_real_escape_string(
      as_mysql(handle_), escaped.data(), reinterpret_cast<const char*>(data),
      static_cast<unsigned long>(length));
  escaped.resize(written);
  return "'" + escaped + "'";
}

std::string SqlSession::render(std::string_view sql, const std::vector<Param>& params) const {
  std::string out;
  out.reserve(sql.size() + 16 * params.size());
  size_t next = 0;
  for (size_t i = 0; i < sql.size(); ++i) {
    if (sql[i] != '?') {
      out += sql[i];
      continue;
    }
    if (next >= params.size()) throw internal_error("more placeholders than parameters");
    const auto& value = params[next++].value;
    if (std::holds_alternative<std::nullptr_t>(value)) {
      out += "NULL";
    } else if (const auto* flag = std::get_if<bool>(&value)) {
      out += *flag ? "TRUE" : "FALSE";
    } else if (const auto* integer = std::get_if<int64_t>(&value)) {
      out += std::to_string(*integer);
    } else if (const auto* number = std::get_if<double>(&value)) {
      // Enough digits to survive the round trip through decimal text.
      char buffer[40];
      snprintf(buffer, sizeof buffer, "%.17g", *number);
      out += buffer;
    } else {
      out += quote(std::get<std::string>(value));
    }
  }
  if (next != params.size()) throw internal_error("more parameters than placeholders");
  return out;
}

void SqlSession::set_identity(const std::string& user, const std::string& host, const std::string& role) {
  if (!identity_) identity_ = new SessionIdentity;
  auto& id = *static_cast<SessionIdentity*>(identity_);
  id.user = user;
  id.host = host;
  THD* thd = session_thd(handle_);
  const LEX_CSTRING u{id.user.c_str(), id.user.size()};
  const LEX_CSTRING h{id.host.c_str(), id.host.size()};
  const LEX_CSTRING empty{"", 0};
#if MYSQL_VERSION_ID < 110000
  const bool missing = acl_getroot(&id.context, u.str, h.str, empty.str, empty.str);
#else
  const bool missing = acl_getroot(&id.context, u, h, empty, empty);
#endif
  if (missing ||
      user != id.context.priv_user || host != id.context.priv_host) {
    throw unauthorized("mapped MariaDB account is unavailable");
  }
  thd->security_ctx = &id.context;
  if (!role.empty()) {
    const LEX_CSTRING r{role.c_str(), role.size()};
    privilege_t access = NO_ACL;
    THD* previous = current_thd;
    set_current_thd(thd);
#if MYSQL_VERSION_ID < 110000
    const bool denied = acl_check_setrole(thd, r.str, &access) || acl_setrole(thd, r.str, access);
#else
    const bool denied = acl_check_setrole(thd, r, &access) || acl_setrole(thd, r, access);
#endif
    set_current_thd(previous);
    if (denied) throw unauthorized("SQL role is unavailable");
  }
}

bool SqlSession::allowed(const std::string& db, const std::string& table, unsigned access) {
  if (!identity_) return true;
  THD* thd = session_thd(handle_);
  privilege_t wanted = NO_ACL;
  if (access & Select) wanted |= SELECT_ACL;
  if (access & Insert) wanted |= INSERT_ACL;
  if (access & Update) wanted |= UPDATE_ACL;
  if (access & Delete) wanted |= DELETE_ACL;
  if (access & Create) wanted |= CREATE_ACL;
  if (access & Drop) wanted |= DROP_ACL;
  if (access & Index) wanted |= INDEX_ACL;
  if (access & Alter) wanted |= ALTER_ACL;
  privilege_t granted = thd->security_ctx->master_access |
                         acl_get_all3(thd->security_ctx, db.c_str(), false);
  if (!table.empty()) {
    const LEX_CSTRING d{db.c_str(), db.size()}, t{table.c_str(), table.size()};
    TABLE_LIST target;
    target.init_one_table(&d, &t, nullptr, TL_READ);
    granted |= get_table_grant(thd, &target);
  }
  return (granted & wanted) == wanted;
}

SqlSession::TableLock::~TableLock() {
  if (ticket_)
    static_cast<THD*>(thd_)->mdl_context.release_lock(static_cast<MDL_ticket*>(ticket_));
}

std::unique_ptr<SqlSession::TableLock> SqlSession::lock_table(
    const std::string& db, const std::string& table, bool writing) {
  THD* thd = session_thd(handle_);
  std::string schema = db, name = table;
  // Match TABLE_LIST's identifier folding, including on case-preserving
  // filesystems. An alternate spelling must not miss the DDL lock.
  if (lower_case_table_names) {
    auto normalize = [](const std::string& value) {
      const auto& charset = my_charset_utf8mb3_general_ci;
      std::string folded(value.size() * charset.casedn_multiply(), '\0');
      folded.resize(charset.casedn(value.data(), value.size(), folded.data(), folded.size()));
      return folded;
    };
    schema = normalize(schema);
    name = normalize(name);
  }
  MDL_request request;
  MDL_REQUEST_INIT(&request, MDL_key::TABLE, schema.c_str(), name.c_str(),
                   writing ? MDL_SHARED_WRITE : MDL_SHARED_READ, MDL_EXPLICIT);
  // Allocate before acquiring a ticket so allocation failure cannot strand an
  // explicit lock on an engine session or a borrowed SQL caller's THD.
  std::unique_ptr<TableLock> lock(new TableLock(thd, nullptr));
  THD* previous = current_thd;
  set_current_thd(thd);
  const bool failed = thd->mdl_context.acquire_lock(&request, thd->variables.lock_wait_timeout);
  set_current_thd(previous);
  if (failed) throw TranslatorError(24, "LockTimeout", "Could not lock collection metadata");
  lock->ticket_ = request.ticket;
  return lock;
}

std::pair<std::string, std::string> SqlSession::caller_identity() {
  if (!current_thd || !current_thd->security_ctx) return {};
  return {current_thd->security_ctx->priv_user, current_thd->security_ctx->priv_host};
}

std::string SqlSession::caller_role() {
  return current_thd && current_thd->security_ctx ? current_thd->security_ctx->priv_role : "";
}

void SqlSession::begin() { exec("START TRANSACTION"); }
void SqlSession::commit() { exec("COMMIT"); }
void SqlSession::rollback() { exec("ROLLBACK"); }

std::string quote_identifier(std::string_view name) {
  if (name.empty()) throw bad_value("empty identifier");
  if (name.find('`') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
    throw bad_value("identifier contains a backtick or NUL: " + std::string(name));
  }
  return "`" + std::string(name) + "`";
}

}  // namespace chimera
