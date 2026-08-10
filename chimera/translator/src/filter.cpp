#include "chimera/filter.h"

#include "chimera/codec.h"
#include "chimera/error.h"

namespace chimera {
namespace {

// Joins compiled fragments, adding parentheses only when they change meaning.
std::string conjoin(const std::vector<std::string>& parts, const char* joiner) {
  if (parts.size() == 1) return parts.front();
  std::string sql;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) sql += joiner;
    sql += parts[i];
  }
  return "(" + sql + ")";
}

// A Timestamp is an ordered pair packed into one 64-bit value, exactly as the
// wire format stores it, so `$gt` on an oplog cursor is a single comparison.
int64_t timestamp_ordinal(const bson_value_t& v) {
  return (static_cast<int64_t>(v.value.v_timestamp.timestamp) << 32) |
         static_cast<int64_t>(v.value.v_timestamp.increment);
}

class Compiler {
public:
  explicit Compiler(std::string doc_column) : doc_(std::move(doc_column)) {}

  std::string compile_document(const bson_t* filter) {
    auto fields = document_fields(filter);
    if (fields.empty()) return "1";
    std::vector<std::string> parts;
    for (const auto& field : fields) parts.push_back(compile_field(field.first, field.second.get()));
    return conjoin(parts, " AND ");
  }

  std::vector<Param> take_params() { return std::move(params_); }

private:
  std::string doc_;
  std::vector<Param> params_;

  std::string value_expr(const Path& path) { return scalar_expr(path, doc_); }

  std::string extract_expr(const Path& path) {
    return "JSON_EXTRACT(" + doc_ + ",'" + to_json_path(path) + "')";
  }

  std::string bind(const bson_value_t& v) {
    switch (v.value_type) {
      case BSON_TYPE_NULL: params_.push_back({nullptr}); break;
      case BSON_TYPE_BOOL: params_.push_back({v.value.v_bool}); break;
      case BSON_TYPE_INT32: params_.push_back({static_cast<int64_t>(v.value.v_int32)}); break;
      case BSON_TYPE_INT64: params_.push_back({v.value.v_int64}); break;
      case BSON_TYPE_DOUBLE: params_.push_back({v.value.v_double}); break;
      case BSON_TYPE_DATE_TIME: params_.push_back({v.value.v_datetime}); break;
      case BSON_TYPE_TIMESTAMP:
        params_.push_back({timestamp_ordinal(v)});
        break;
      case BSON_TYPE_OID: {
        char oid[25];
        bson_oid_to_string(&v.value.v_oid, oid);
        params_.push_back({std::string(oid)});
        break;
      }
      case BSON_TYPE_UTF8:
        params_.push_back({std::string(v.value.v_utf8.str, v.value.v_utf8.len)});
        break;
      default:
        throw not_implemented("filter comparison against this BSON type is not supported");
    }
    return "?";
  }

  std::string bind_string(std::string s) {
    params_.push_back({std::move(s)});
    return "?";
  }

  // JSON_VALUE returns text; numeric comparisons must compare as numbers.
  std::string comparable(const Path& path, const bson_value_t& against) {
    if (against.value_type == BSON_TYPE_TIMESTAMP) {
      // A Timestamp orders by (t, i) as one 64-bit ordinal, which a DOUBLE
      // cannot hold exactly — hence integer arithmetic rather than a CAST of
      // the COALESCE that scalar_expr builds.
      const std::string base = to_json_path(path);
      const auto part = [&](const char* name) {
        return "CAST(JSON_VALUE(" + doc_ + ",'" + base + R"(."$timestamp".)" + name +
               "') AS UNSIGNED)";
      };
      return "(" + part("t") + " * 4294967296 + " + part("i") + ")";
    }
    if (value_is_numeric(against) || against.value_type == BSON_TYPE_DATE_TIME) {
      return "CAST(" + value_expr(path) + " AS DOUBLE)";
    }
    return value_expr(path);
  }

  std::string compile_logical(const char* joiner, const bson_value_t& v) {
    auto branches = array_values(v);
    if (branches.empty()) throw failed_to_parse("logical operator requires a non-empty array");
    std::vector<std::string> parts;
    for (const auto& branch : branches) {
      bson_t view;
      if (branch.type() != BSON_TYPE_DOCUMENT ||
          !bson_init_static(&view, branch.get().value.v_doc.data,
                            branch.get().value.v_doc.data_len)) {
        throw failed_to_parse("logical operator branches must be documents");
      }
      parts.push_back(compile_document(&view));
    }
    return conjoin(parts, joiner);
  }

  std::string compile_field(const std::string& key, const bson_value_t& v) {
    if (key == "$and") return compile_logical(" AND ", v);
    if (key == "$or") return compile_logical(" OR ", v);
    if (key == "$nor") return "NOT (" + compile_logical(" OR ", v) + ")";
    if (!key.empty() && key[0] == '$') {
      throw not_implemented("unsupported top-level operator '" + key + "'");
    }

    Path path = split_path(key);
    bson_t view;
    if (v.value_type == BSON_TYPE_DOCUMENT &&
        bson_init_static(&view, v.value.v_doc.data, v.value.v_doc.data_len)) {
      auto ops = document_fields(&view);
      bool operator_document =
          !ops.empty() && !ops.front().first.empty() && ops.front().first[0] == '$';
      if (operator_document) return compile_operators(path, ops);
    }
    return compile_eq(path, v);
  }

  // The stored document is canonical Extended JSON, so a containment candidate
  // has to be canonical too — 1 is stored as {"$numberInt":"1"}, not 1.
  // Serializing the value inside a throwaway wrapper is the only way libbson
  // will emit a bare value in that encoding.
  std::string canonical_literal(const bson_value_t& v) {
    Bson wrapper;
    bson_append_value(wrapper.get(), "v", 1, &v);
    std::string json = to_extjson(wrapper.get());
    size_t begin = json.find(':') + 1;
    size_t end = json.rfind('}');
    std::string literal = json.substr(begin, end - begin);
    size_t first = literal.find_first_not_of(" \t\n");
    size_t last = literal.find_last_not_of(" \t\n");
    return literal.substr(first, last - first + 1);
  }

  std::string compile_eq(const Path& path, const bson_value_t& v) {
    if (v.value_type == BSON_TYPE_NULL) return value_expr(path) + " IS NULL";
    // A bare regex value is a match, not an equality — {name: /^Do/}.
    if (v.value_type == BSON_TYPE_REGEX) {
      return value_expr(path) + " REGEXP " + bind_string(v.value.v_regex.regex);
    }
    if (v.value_type == BSON_TYPE_DOCUMENT || v.value_type == BSON_TYPE_ARRAY) {
      throw not_implemented("equality against a document or array is not supported");
    }
    // A scalar also matches when the field holds an array containing it —
    // minimongo relies on this for tag-style fields.
    std::string eq = comparable(path, v) + " = " + bind(v);
    std::string contains =
        "JSON_CONTAINS(" + extract_expr(path) + ", " + bind_string(canonical_literal(v)) + ")";
    return "(" + eq + " OR " + contains + ")";
  }

  std::string compile_operators(const Path& path,
                                const std::vector<std::pair<std::string, Value>>& ops) {
    std::vector<std::string> parts;
    for (const auto& op : ops) parts.push_back(compile_operator(path, op.first, op.second.get()));
    return conjoin(parts, " AND ");
  }

  std::string compile_in(const Path& path, const bson_value_t& v, bool negated) {
    auto members = array_values(v);
    if (members.empty()) return negated ? "1" : "0";
    std::string list;
    for (size_t i = 0; i < members.size(); ++i) {
      if (i) list += ", ";
      list += bind(members[i].get());
    }
    return value_expr(path) + (negated ? " NOT IN (" : " IN (") + list + ")";
  }

  std::string compile_operator(const Path& path, const std::string& op, const bson_value_t& v) {
    if (op == "$eq") return compile_eq(path, v);
    if (op == "$ne") return "NOT (" + compile_eq(path, v) + ")";
    if (op == "$gt") return comparable(path, v) + " > " + bind(v);
    if (op == "$gte") return comparable(path, v) + " >= " + bind(v);
    if (op == "$lt") return comparable(path, v) + " < " + bind(v);
    if (op == "$lte") return comparable(path, v) + " <= " + bind(v);
    if (op == "$in") return compile_in(path, v, false);
    if (op == "$nin") return compile_in(path, v, true);
    if (op == "$exists") {
      bool want = v.value_type == BSON_TYPE_BOOL ? v.value.v_bool : true;
      std::string test = "JSON_EXISTS(" + doc_ + ",'" + to_json_path(path) + "')";
      return want ? test : "NOT " + test;
    }
    if (op == "$regex") {
      if (v.value_type == BSON_TYPE_UTF8) {
        return value_expr(path) + " REGEXP " + bind(v);
      }
      if (v.value_type == BSON_TYPE_REGEX) {
        return value_expr(path) + " REGEXP " + bind_string(v.value.v_regex.regex);
      }
      throw failed_to_parse("$regex expects a string or regex");
    }
    if (op == "$not") {
      bson_t view;
      if (v.value_type != BSON_TYPE_DOCUMENT ||
          !bson_init_static(&view, v.value.v_doc.data, v.value.v_doc.data_len)) {
        throw failed_to_parse("$not expects an operator document");
      }
      return "NOT (" + compile_operators(path, document_fields(&view)) + ")";
    }
    if (op == "$elemMatch") {
      // Basic form only: whole-element containment. Per-element operator
      // documents need a lateral join and are deferred (M8).
      if (v.value_type != BSON_TYPE_DOCUMENT) {
        throw failed_to_parse("$elemMatch expects a document");
      }
      bson_t view;
      if (!bson_init_static(&view, v.value.v_doc.data, v.value.v_doc.data_len)) {
        throw failed_to_parse("$elemMatch document is malformed");
      }
      for (const auto& field : document_fields(&view)) {
        if (!field.first.empty() && field.first[0] == '$') {
          throw not_implemented("$elemMatch with operators is not supported yet");
        }
      }
      return "JSON_CONTAINS(" + extract_expr(path) + ", " + bind_string(to_extjson(&view)) + ")";
    }
    // MongoDB does not distinguish "unimplemented" from "misspelled" for a
    // query operator it has never heard of; both are BadValue.
    throw bad_value("unknown operator: " + op);
  }
};

}  // namespace

std::string scalar_expr(const Path& path, const std::string& doc_column) {
  // JSON_VALUE returns NULL for a non-scalar, and in canonical extJSON every
  // non-string scalar *is* an object: 30 is stored as {"$numberInt":"30"}. So
  // reach for the bare value first, then through each wrapper we can compare
  // meaningfully. The first non-NULL wins, which is unambiguous because a given
  // value has exactly one encoding.
  static const char* const kWrappers[] = {
      "",  // strings, booleans, and JSON null
      R"(."$numberInt")", R"(."$numberLong")", R"(."$numberDouble")",
      R"(."$date"."$numberLong")",  // dates compare as epoch milliseconds
      R"(."$oid")",
  };
  const std::string base = to_json_path(path);
  std::string out = "COALESCE(";
  for (size_t i = 0; i < sizeof kWrappers / sizeof kWrappers[0]; ++i) {
    if (i > 0) out += ",";
    out += "JSON_VALUE(" + doc_column + ",'" + base + kWrappers[i] + "')";
  }
  return out + ")";
}

SqlFilter compile_filter(const bson_t* filter, const std::string& doc_column) {
  Compiler compiler(doc_column);
  SqlFilter out;
  out.sql = compiler.compile_document(filter);
  out.params = compiler.take_params();
  return out;
}

}  // namespace chimera
