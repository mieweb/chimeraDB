#include "chimera/bson.h"

#include <cstring>

#include "chimera/error.h"

namespace chimera {

Bson::Bson() : b_(bson_new()) {}
Bson::Bson(bson_t* owned) noexcept : b_(owned) {}
Bson::Bson(Bson&& other) noexcept : b_(other.b_) { other.b_ = nullptr; }

Bson& Bson::operator=(Bson&& other) noexcept {
  if (this != &other) {
    if (b_) bson_destroy(b_);
    b_ = other.b_;
    other.b_ = nullptr;
  }
  return *this;
}

Bson::~Bson() {
  if (b_) bson_destroy(b_);
}

Bson Bson::copy_of(const bson_t* src) { return Bson(bson_copy(src)); }

bson_t* Bson::release() noexcept {
  bson_t* out = b_;
  b_ = nullptr;
  return out;
}

Value::Value() { std::memset(&v_, 0, sizeof(v_)); v_.value_type = BSON_TYPE_NULL; }

Value::Value(const bson_value_t& v) { bson_value_copy(&v, &v_); }

Value::Value(const Value& other) { bson_value_copy(&other.v_, &v_); }

Value& Value::operator=(const Value& other) {
  if (this != &other) {
    bson_value_destroy(&v_);
    bson_value_copy(&other.v_, &v_);
  }
  return *this;
}

Value::Value(Value&& other) noexcept : v_(other.v_) {
  std::memset(&other.v_, 0, sizeof(other.v_));
  other.v_.value_type = BSON_TYPE_EOD;
}

Value& Value::operator=(Value&& other) noexcept {
  if (this != &other) {
    bson_value_destroy(&v_);
    v_ = other.v_;
    std::memset(&other.v_, 0, sizeof(other.v_));
    other.v_.value_type = BSON_TYPE_EOD;
  }
  return *this;
}

Value::~Value() { bson_value_destroy(&v_); }

Value Value::from_int64(int64_t n) {
  bson_value_t v;
  v.value_type = BSON_TYPE_INT64;
  v.value.v_int64 = n;
  return Value(v);
}

Value Value::from_double(double d) {
  bson_value_t v;
  v.value_type = BSON_TYPE_DOUBLE;
  v.value.v_double = d;
  return Value(v);
}

Value Value::from_utf8(std::string_view s) {
  bson_value_t v;
  v.value_type = BSON_TYPE_UTF8;
  v.value.v_utf8.str = const_cast<char*>(s.data());
  v.value.v_utf8.len = static_cast<uint32_t>(s.size());
  return Value(v);  // bson_value_copy takes its own copy of the bytes
}

static Value value_from_buffer(bson_type_t type, const bson_t* doc) {
  bson_value_t v;
  v.value_type = type;
  v.value.v_doc.data = const_cast<uint8_t*>(bson_get_data(doc));
  v.value.v_doc.data_len = doc->len;
  return Value(v);
}

Value Value::from_document(const bson_t* doc) { return value_from_buffer(BSON_TYPE_DOCUMENT, doc); }
Value Value::from_array(const bson_t* arr) { return value_from_buffer(BSON_TYPE_ARRAY, arr); }

bool value_is_numeric(const bson_value_t& v) {
  return v.value_type == BSON_TYPE_INT32 || v.value_type == BSON_TYPE_INT64 ||
         v.value_type == BSON_TYPE_DOUBLE;
}

double value_as_double(const bson_value_t& v) {
  switch (v.value_type) {
    case BSON_TYPE_INT32: return static_cast<double>(v.value.v_int32);
    case BSON_TYPE_INT64: return static_cast<double>(v.value.v_int64);
    case BSON_TYPE_DOUBLE: return v.value.v_double;
    default: throw type_mismatch("expected a numeric value");
  }
}

bool value_equal(const bson_value_t& a, const bson_value_t& b) {
  // Numbers compare numerically across int32/int64/double, the way MongoDB
  // treats them; everything else must match type as well as payload.
  if (value_is_numeric(a) && value_is_numeric(b)) {
    return value_as_double(a) == value_as_double(b);
  }
  if (a.value_type != b.value_type) return false;
  switch (a.value_type) {
    case BSON_TYPE_UTF8:
      return a.value.v_utf8.len == b.value.v_utf8.len &&
             std::memcmp(a.value.v_utf8.str, b.value.v_utf8.str, a.value.v_utf8.len) == 0;
    case BSON_TYPE_BOOL:
      return a.value.v_bool == b.value.v_bool;
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
    case BSON_TYPE_MINKEY:
    case BSON_TYPE_MAXKEY:
      return true;
    case BSON_TYPE_OID:
      return bson_oid_equal(&a.value.v_oid, &b.value.v_oid);
    case BSON_TYPE_DATE_TIME:
      return a.value.v_datetime == b.value.v_datetime;
    case BSON_TYPE_TIMESTAMP:
      return a.value.v_timestamp.timestamp == b.value.v_timestamp.timestamp &&
             a.value.v_timestamp.increment == b.value.v_timestamp.increment;
    case BSON_TYPE_BINARY:
      return a.value.v_binary.data_len == b.value.v_binary.data_len &&
             std::memcmp(a.value.v_binary.data, b.value.v_binary.data,
                         a.value.v_binary.data_len) == 0;
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
      return a.value.v_doc.data_len == b.value.v_doc.data_len &&
             std::memcmp(a.value.v_doc.data, b.value.v_doc.data, a.value.v_doc.data_len) == 0;
    default:
      return false;
  }
}

Path split_path(std::string_view dotted) {
  Path parts;
  size_t start = 0;
  while (true) {
    size_t dot = dotted.find('.', start);
    if (dot == std::string_view::npos) {
      parts.emplace_back(dotted.substr(start));
      break;
    }
    parts.emplace_back(dotted.substr(start, dot - start));
    start = dot + 1;
  }
  for (const auto& part : parts) {
    if (part.empty()) throw failed_to_parse("empty path member in '" + std::string(dotted) + "'");
  }
  return parts;
}

std::string to_json_path(const Path& path) {
  std::string out = "$";
  for (const auto& member : path) {
    out += ".\"";
    for (char c : member) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    out += '"';
  }
  return out;
}

// Read a subdocument/array out of an iterator without copying it.
static bool iter_subdocument(const bson_iter_t& it, bson_t* out) {
  bson_type_t type = bson_iter_type(&it);
  if (type != BSON_TYPE_DOCUMENT && type != BSON_TYPE_ARRAY) return false;
  const uint8_t* data = nullptr;
  uint32_t len = 0;
  bson_iter_t copy = it;
  if (type == BSON_TYPE_DOCUMENT) {
    bson_iter_document(&copy, &len, &data);
  } else {
    bson_iter_array(&copy, &len, &data);
  }
  return bson_init_static(out, data, len);
}

std::optional<Value> path_get(const bson_t* doc, const Path& path) {
  const bson_t* current = doc;
  bson_t child;
  for (size_t i = 0; i < path.size(); ++i) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, current, path[i].c_str())) return std::nullopt;
    if (i + 1 == path.size()) return Value(*bson_iter_value(&it));
    if (!iter_subdocument(it, &child)) return std::nullopt;
    current = &child;
  }
  return std::nullopt;
}

static void append_value(bson_t* out, const std::string& key, const bson_value_t& value) {
  bson_append_value(out, key.c_str(), static_cast<int>(key.size()), &value);
}

static void append_subdocument(bson_t* out, const std::string& key, const Bson& sub, bool as_array) {
  if (as_array) {
    bson_append_array(out, key.c_str(), static_cast<int>(key.size()), sub.get());
  } else {
    bson_append_document(out, key.c_str(), static_cast<int>(key.size()), sub.get());
  }
}

static Bson set_in(const bson_t* src, const Path& path, size_t depth, const bson_value_t& value) {
  Bson out;
  bool handled = false;
  bson_iter_t it;
  if (src && bson_iter_init(&it, src)) {
    while (bson_iter_next(&it)) {
      std::string key = bson_iter_key(&it);
      if (key != path[depth]) {
        append_value(out.get(), key, *bson_iter_value(&it));
        continue;
      }
      handled = true;
      if (depth + 1 == path.size()) {
        append_value(out.get(), key, value);
      } else {
        bson_t child;
        bool is_array = bson_iter_type(&it) == BSON_TYPE_ARRAY;
        bool descended = iter_subdocument(it, &child);
        Bson sub = set_in(descended ? &child : nullptr, path, depth + 1, value);
        append_subdocument(out.get(), key, sub, descended && is_array);
      }
    }
  }
  if (!handled) {
    if (depth + 1 == path.size()) {
      append_value(out.get(), path[depth], value);
    } else {
      Bson sub = set_in(nullptr, path, depth + 1, value);
      append_subdocument(out.get(), path[depth], sub, false);
    }
  }
  return out;
}

Bson path_set(const bson_t* doc, const Path& path, const bson_value_t& value) {
  if (path.empty()) throw failed_to_parse("empty path");
  return set_in(doc, path, 0, value);
}

static Bson unset_in(const bson_t* src, const Path& path, size_t depth) {
  Bson out;
  bson_iter_t it;
  if (src && bson_iter_init(&it, src)) {
    while (bson_iter_next(&it)) {
      std::string key = bson_iter_key(&it);
      if (key != path[depth]) {
        append_value(out.get(), key, *bson_iter_value(&it));
        continue;
      }
      if (depth + 1 == path.size()) continue;  // dropped
      bson_t child;
      bool is_array = bson_iter_type(&it) == BSON_TYPE_ARRAY;
      if (!iter_subdocument(it, &child)) {
        append_value(out.get(), key, *bson_iter_value(&it));
        continue;
      }
      Bson sub = unset_in(&child, path, depth + 1);
      append_subdocument(out.get(), key, sub, is_array);
    }
  }
  return out;
}

Bson path_unset(const bson_t* doc, const Path& path) {
  if (path.empty()) throw failed_to_parse("empty path");
  return unset_in(doc, path, 0);
}

std::vector<Value> array_elements(const bson_value_t& array) {
  if (array.value_type != BSON_TYPE_ARRAY) throw type_mismatch("expected an array");
  bson_t view;
  if (!bson_init_static(&view, array.value.v_doc.data, array.value.v_doc.data_len)) {
    throw bad_value("malformed array");
  }
  std::vector<Value> out;
  bson_iter_t it;
  if (bson_iter_init(&it, &view)) {
    while (bson_iter_next(&it)) out.emplace_back(*bson_iter_value(&it));
  }
  return out;
}

Bson make_array(const std::vector<Value>& elements) {
  Bson out;
  for (size_t i = 0; i < elements.size(); ++i) {
    append_value(out.get(), std::to_string(i), elements[i].get());
  }
  return out;
}

std::vector<std::pair<std::string, Value>> document_fields(const bson_t* doc) {
  std::vector<std::pair<std::string, Value>> out;
  bson_iter_t it;
  if (doc && bson_iter_init(&it, doc)) {
    while (bson_iter_next(&it)) out.emplace_back(bson_iter_key(&it), Value(*bson_iter_value(&it)));
  }
  return out;
}

}  // namespace chimera
