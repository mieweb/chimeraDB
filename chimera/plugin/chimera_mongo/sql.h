#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chimera/filter.h"

namespace chimera {

// A NULL column is an empty optional; everything else arrives as bytes, because
// that is all the server's local-connection protocol offers.
using Row = std::vector<std::optional<std::string>>;

// What the server says a column holds. Since every value crosses as bytes, this
// is the only way a caller can put a number back where it came from as a number
// rather than as its decimal spelling.
enum class ColumnType { Text, Integer, Real, Bool };

struct Column {
  std::string name;
  ColumnType type = ColumnType::Text;
};

struct ResultSet {
  std::vector<Column> columns;
  std::vector<Row> rows;
};

// A connection to the server this plugin lives inside — the single place where
// chimera turns intent into SQL. One session per mongo connection, so each
// client gets its own THD and therefore its own transaction context.
//
// The handle is void* on purpose: MariaDB's headers redefine common words
// (`array_elements` among them) and must not leak into the rest of the plugin.
class SqlSession {
public:
  SqlSession();
  ~SqlSession();
  SqlSession(const SqlSession&) = delete;
  SqlSession& operator=(const SqlSession&) = delete;

  void exec(const std::string& sql);
  ResultSet query(const std::string& sql);
  uint64_t affected_rows() const;

  // The one and only place a Param becomes SQL text. The local connection has
  // no prepared-statement API, so `?` is substituted here using the server's
  // own escaper — keeping a single audited choke point instead of scattering
  // string concatenation through the command handlers.
  std::string render(std::string_view sql, const std::vector<Param>& params) const;
  std::string quote(std::string_view text) const;
  std::string quote_binary(const uint8_t* data, size_t length) const;

  void begin();
  void commit();
  void rollback();

private:
  void* handle_ = nullptr;  // MYSQL*
};

// mysys keeps per-thread state that the server dereferences on the first query,
// so any thread owning a SqlSession must be registered for its whole lifetime.
class SqlThreadScope {
public:
  SqlThreadScope();
  ~SqlThreadScope();
  SqlThreadScope(const SqlThreadScope&) = delete;
  SqlThreadScope& operator=(const SqlThreadScope&) = delete;
};

// Backtick-quotes an identifier after rejecting anything that could escape it.
std::string quote_identifier(std::string_view name);

}  // namespace chimera
