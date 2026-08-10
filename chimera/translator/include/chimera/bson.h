#pragma once

#include <bson/bson.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chimera {

// Owning, move-only handle for a bson_t. Documents travel through the
// translator as these so no caller has to remember who calls bson_destroy.
class Bson {
public:
  Bson();
  explicit Bson(bson_t* owned) noexcept;
  Bson(Bson&& other) noexcept;
  Bson& operator=(Bson&& other) noexcept;
  Bson(const Bson&) = delete;
  Bson& operator=(const Bson&) = delete;
  ~Bson();

  static Bson copy_of(const bson_t* src);

  const bson_t* get() const noexcept { return b_; }
  bson_t* get() noexcept { return b_; }
  bson_t* release() noexcept;

private:
  bson_t* b_;
};

// Owning copy of a single BSON value.
class Value {
public:
  Value();
  explicit Value(const bson_value_t& v);
  Value(const Value& other);
  Value& operator=(const Value& other);
  Value(Value&& other) noexcept;
  Value& operator=(Value&& other) noexcept;
  ~Value();

  const bson_value_t& get() const noexcept { return v_; }
  bson_type_t type() const noexcept { return v_.value_type; }

  static Value from_int64(int64_t n);
  static Value from_double(double d);
  static Value from_utf8(std::string_view s);
  static Value from_document(const bson_t* doc);
  static Value from_array(const bson_t* arr);

private:
  bson_value_t v_;
};

bool value_equal(const bson_value_t& a, const bson_value_t& b);
bool value_is_numeric(const bson_value_t& v);
double value_as_double(const bson_value_t& v);

// A document path split into members. A member that is all digits addresses an
// array element; everything else is a field name.
using Path = std::vector<std::string>;
Path split_path(std::string_view dotted);

// MariaDB JSON path for a document path. Every member is quoted, which keeps
// Extended JSON's "$"-prefixed keys ($date, $numberLong, …) addressable.
std::string to_json_path(const Path& path);

std::optional<Value> path_get(const bson_t* doc, const Path& path);
// Missing intermediate levels are created as documents.
Bson path_set(const bson_t* doc, const Path& path, const bson_value_t& value);
Bson path_unset(const bson_t* doc, const Path& path);

// BSON arrays are documents keyed "0","1",… — these two hide that.
std::vector<Value> array_elements(const bson_value_t& array);
Bson make_array(const std::vector<Value>& elements);

// Iterate a document as (key, value) pairs, in document order.
std::vector<std::pair<std::string, Value>> document_fields(const bson_t* doc);

}  // namespace chimera
