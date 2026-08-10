#include "chimera/sort.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "chimera/error.h"

namespace chimera {
namespace {

// The publicly documented BSON comparison order. Types that share a rank
// compare against each other by value — all three numeric types are one rank,
// which is what makes 1 == NumberLong(1) in a sort.
int type_rank(bson_type_t type) {
  switch (type) {
    case BSON_TYPE_MINKEY: return 0;
    case BSON_TYPE_EOD:
    case BSON_TYPE_UNDEFINED:
    case BSON_TYPE_NULL: return 1;
    case BSON_TYPE_INT32:
    case BSON_TYPE_INT64:
    case BSON_TYPE_DOUBLE:
    case BSON_TYPE_DECIMAL128: return 2;
    case BSON_TYPE_UTF8:
    case BSON_TYPE_SYMBOL: return 3;
    case BSON_TYPE_DOCUMENT: return 4;
    case BSON_TYPE_ARRAY: return 5;
    case BSON_TYPE_BINARY: return 6;
    case BSON_TYPE_OID: return 7;
    case BSON_TYPE_BOOL: return 8;
    case BSON_TYPE_DATE_TIME: return 9;
    case BSON_TYPE_TIMESTAMP: return 10;
    case BSON_TYPE_REGEX: return 11;
    case BSON_TYPE_MAXKEY: return 12;
    default: return 4;
  }
}

int sign(int64_t n) { return n < 0 ? -1 : (n > 0 ? 1 : 0); }

int compare_bytes(const uint8_t* a, uint32_t a_len, const uint8_t* b, uint32_t b_len) {
  const uint32_t shared = a_len < b_len ? a_len : b_len;
  if (shared > 0) {
    const int diff = memcmp(a, b, shared);
    if (diff != 0) return diff < 0 ? -1 : 1;
  }
  return sign(static_cast<int64_t>(a_len) - static_cast<int64_t>(b_len));
}

int compare_documents(const bson_value_t& left, const bson_value_t& right, bool as_array);

}  // namespace

int compare_values(const bson_value_t& left, const bson_value_t& right) {
  const int left_rank = type_rank(left.value_type);
  const int right_rank = type_rank(right.value_type);
  if (left_rank != right_rank) return left_rank < right_rank ? -1 : 1;

  switch (left_rank) {
    case 2: {  // numbers, compared as doubles like the server does
      const double a = value_as_double(left);
      const double b = value_as_double(right);
      return a < b ? -1 : (a > b ? 1 : 0);
    }
    case 3:
      return compare_bytes(reinterpret_cast<const uint8_t*>(left.value.v_utf8.str),
                           left.value.v_utf8.len,
                           reinterpret_cast<const uint8_t*>(right.value.v_utf8.str),
                           right.value.v_utf8.len);
    case 4:
      return compare_documents(left, right, false);
    case 5:
      return compare_documents(left, right, true);
    case 6:
      return compare_bytes(left.value.v_binary.data, left.value.v_binary.data_len,
                           right.value.v_binary.data, right.value.v_binary.data_len);
    case 7:
      return sign(bson_oid_compare(&left.value.v_oid, &right.value.v_oid));
    case 8:
      return sign(static_cast<int>(left.value.v_bool) - static_cast<int>(right.value.v_bool));
    case 9:
      return sign(left.value.v_datetime - right.value.v_datetime);
    case 10: {
      if (left.value.v_timestamp.timestamp != right.value.v_timestamp.timestamp) {
        return left.value.v_timestamp.timestamp < right.value.v_timestamp.timestamp ? -1 : 1;
      }
      return sign(static_cast<int64_t>(left.value.v_timestamp.increment) -
                  static_cast<int64_t>(right.value.v_timestamp.increment));
    }
    case 11:
      return strcmp(left.value.v_regex.regex, right.value.v_regex.regex) < 0 ? -1 : 0;
    default:
      return 0;  // null, minkey, maxkey: every instance is equal to every other
  }
}

namespace {

int compare_documents(const bson_value_t& left, const bson_value_t& right, bool as_array) {
  bson_t a;
  bson_t b;
  if (!bson_init_static(&a, left.value.v_doc.data, left.value.v_doc.data_len) ||
      !bson_init_static(&b, right.value.v_doc.data, right.value.v_doc.data_len)) {
    return 0;
  }
  auto left_fields = document_fields(&a);
  auto right_fields = document_fields(&b);
  const size_t shared = std::min(left_fields.size(), right_fields.size());
  for (size_t i = 0; i < shared; ++i) {
    // Arrays compare element-wise; documents compare key-then-value, because in
    // a document the field name is part of the value's identity.
    if (!as_array && left_fields[i].first != right_fields[i].first) {
      return left_fields[i].first < right_fields[i].first ? -1 : 1;
    }
    const int diff = compare_values(left_fields[i].second.get(), right_fields[i].second.get());
    if (diff != 0) return diff;
  }
  return sign(static_cast<int64_t>(left_fields.size()) -
              static_cast<int64_t>(right_fields.size()));
}

}  // namespace

void sort_documents(std::vector<Bson>& documents, const bson_t* spec) {
  if (spec == nullptr) return;
  auto keys = document_fields(spec);
  if (keys.empty()) return;

  struct Key {
    Path path;
    int direction;
  };
  std::vector<Key> sort_keys;
  for (const auto& [name, value] : keys) {
    const double direction = value_is_numeric(value.get()) ? value_as_double(value.get()) : 0;
    if (direction != 1 && direction != -1) {
      throw bad_value("sort direction for '" + name + "' must be 1 or -1");
    }
    sort_keys.push_back({split_path(name), direction > 0 ? 1 : -1});
  }

  // A field that is absent sorts exactly where an explicit null would.
  bson_value_t missing;
  memset(&missing, 0, sizeof missing);
  missing.value_type = BSON_TYPE_NULL;

  std::stable_sort(documents.begin(), documents.end(),
                   [&](const Bson& left, const Bson& right) {
                     for (const auto& key : sort_keys) {
                       auto a = path_get(left.get(), key.path);
                       auto b = path_get(right.get(), key.path);
                       const int diff = compare_values(a ? a->get() : missing,
                                                       b ? b->get() : missing);
                       if (diff != 0) return key.direction > 0 ? diff < 0 : diff > 0;
                     }
                     return false;
                   });
}

}  // namespace chimera
