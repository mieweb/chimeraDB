// SQL for clients that only speak Mongo.
//
// The interesting part is not running the statement — it is refusing to run the
// wrong one. Writes are off unless a DBA turns them on, and that is enforced
// twice: a keyword whitelist, and the server's own read-only transaction. The
// two catch different things, which is why both are here.
#include "sqlgateway.h"

#include <cstdlib>

#include "chimera/error.h"
#include "chimera/sqlguard.h"

namespace chimera {
namespace {

// Points at the plugin's system variable; null until the plugin has started,
// which is the safe reading — no flag, no writes.
const char* g_write_flag = nullptr;

bool writes_allowed() { return g_write_flag != nullptr && *g_write_flag != 0; }

void append_column(bson_t* doc, const Column& column, const std::optional<std::string>& value) {
  const char* key = column.name.c_str();
  const int key_length = static_cast<int>(column.name.size());
  if (!value) {
    bson_append_null(doc, key, key_length);
    return;
  }
  switch (column.type) {
    case ColumnType::Integer:
      bson_append_int64(doc, key, key_length, std::strtoll(value->c_str(), nullptr, 10));
      return;
    case ColumnType::Real:
      bson_append_double(doc, key, key_length, std::strtod(value->c_str(), nullptr));
      return;
    case ColumnType::Bool:
      bson_append_bool(doc, key, key_length, *value != "0");
      return;
    case ColumnType::Text:
      bson_append_utf8(doc, key, key_length, value->data(), static_cast<int>(value->size()));
      return;
  }
}

}  // namespace

void set_sql_gateway_write_flag(const char* flag) { g_write_flag = flag; }

SqlGatewayResult run_sql_gateway(SqlSession& sql, const std::string& statement) {
  if (statement.empty()) throw failed_to_parse("chimeraSql needs a statement");

  const bool read_only = !writes_allowed();
  if (read_only && !is_read_only_statement(statement)) {
    throw unauthorized(
        "the SQL gateway is read-only; '" + leading_keyword(statement) +
        "' needs SET GLOBAL chimera_mongo_sql_writes = ON");
  }

  // The second defence. A read-only transaction is the server's own judgement
  // about what writes, applied to statements the keyword check let through —
  // a view over a table with a trigger, say, or a function with a side effect.
  // It cannot replace the keyword check, because DDL commits implicitly before
  // it runs and so never becomes part of the transaction at all.
  if (read_only) sql.exec("START TRANSACTION READ ONLY");
  SqlGatewayResult result;
  try {
    const ResultSet rows = sql.query(statement);
    result.had_result_set = !rows.columns.empty();
    for (const auto& row : rows.rows) {
      Bson document;
      for (size_t i = 0; i < rows.columns.size() && i < row.size(); ++i) {
        append_column(document.get(), rows.columns[i], row[i]);
      }
      result.documents.push_back(std::move(document));
    }
    result.affected_rows = sql.affected_rows();
  } catch (...) {
    if (read_only) sql.rollback();
    throw;
  }
  if (read_only) sql.commit();
  return result;
}

}  // namespace chimera
