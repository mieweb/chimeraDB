// The only translation unit besides chimera_mongo.cc that sees MariaDB headers.
#include <my_global.h>

#include <mysql.h>
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

// Server error numbers carry meaning the mongo client needs; anything we do not
// map deliberately surfaces as InternalError with the server's own text, which
// is more useful than a guessed mongo code.
[[noreturn]] void raise(MYSQL* mysql, const std::string& sql) {
  const unsigned code = mysql_errno(mysql);
  const std::string detail = std::string(mysql_error(mysql)) + " [" + sql.substr(0, 400) + "]";
  switch (code) {
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

SqlSession::SqlSession() {
  MYSQL* mysql = mysql_init(nullptr);
  if (mysql == nullptr) throw internal_error("mysql_init failed");
  if (mysql_real_connect_local(mysql) == nullptr) {
    mysql_close(mysql);
    throw internal_error("could not open a local connection to the server");
  }
  handle_ = mysql;
  // Documents are stored as extJSON text, so the session must speak the same
  // charset the collection tables use or multi-byte content round-trips wrong.
  mysql_set_character_set(mysql, "utf8mb4");
}

SqlSession::~SqlSession() {
  if (handle_ != nullptr) mysql_close(as_mysql(handle_));
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
  for (unsigned i = 0; i < field_count; ++i) out.columns.emplace_back(fields[i].name);

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
