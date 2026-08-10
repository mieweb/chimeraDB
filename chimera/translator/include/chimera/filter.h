#pragma once

#include <string>
#include <variant>
#include <vector>

#include "chimera/bson.h"

namespace chimera {

// A value bound to a `?` placeholder. User data never reaches the SQL string —
// that is the whole injection defence, so there is no "inline literal" variant
// here on purpose.
struct Param {
  std::variant<std::nullptr_t, bool, int64_t, double, std::string> value;
};

struct SqlFilter {
  std::string sql;             // a boolean expression over the `doc` column
  std::vector<Param> params;   // one per '?' in sql, in order
};

// Compiles the Meteor/minimongo operator subset. Anything outside it raises,
// so a query can never quietly return the wrong rows.
SqlFilter compile_filter(const bson_t* filter, const std::string& doc_column = "doc");

// The SQL expression that reads one scalar field out of a stored document.
// Exported because an index's generated column has to compute exactly what the
// WHERE clause computes, or the optimizer will never use it.
std::string scalar_expr(const Path& path, const std::string& doc_column = "doc");

}  // namespace chimera
