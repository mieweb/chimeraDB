#include "chimera/id.h"

#include <cstring>

#include "chimera/error.h"

namespace chimera {
namespace {

constexpr char kTagString = static_cast<char>(BSON_TYPE_UTF8);   // 0x02
constexpr char kTagOid = static_cast<char>(BSON_TYPE_OID);       // 0x07
constexpr char kTagInt = static_cast<char>(BSON_TYPE_INT64);     // 0x12

// Flipping the sign bit makes two's-complement integers sort correctly as
// unsigned big-endian bytes, so InnoDB's key order matches numeric order.
std::string encode_int(int64_t n) {
  uint64_t biased = static_cast<uint64_t>(n) ^ (1ULL << 63);
  std::string out(8, '\0');
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>((biased >> (56 - 8 * i)) & 0xFF);
  return out;
}

int64_t decode_int(const std::string& bytes) {
  uint64_t biased = 0;
  for (int i = 0; i < 8; ++i) biased = (biased << 8) | static_cast<uint8_t>(bytes[i]);
  return static_cast<int64_t>(biased ^ (1ULL << 63));
}

}  // namespace

std::string encode_id(const bson_value_t& id) {
  switch (id.value_type) {
    case BSON_TYPE_OID:
      return std::string(1, kTagOid) +
             std::string(reinterpret_cast<const char*>(id.value.v_oid.bytes), 12);
    case BSON_TYPE_UTF8:
      return std::string(1, kTagString) + std::string(id.value.v_utf8.str, id.value.v_utf8.len);
    case BSON_TYPE_INT32:
      return std::string(1, kTagInt) + encode_int(id.value.v_int32);
    case BSON_TYPE_INT64:
      return std::string(1, kTagInt) + encode_int(id.value.v_int64);
    default:
      throw type_mismatch("unsupported _id type — expected ObjectId, string, or integer");
  }
}

Value decode_id(const std::string& key) {
  if (key.empty()) throw bad_value("empty _id key");
  std::string payload = key.substr(1);
  bson_value_t v;
  std::memset(&v, 0, sizeof(v));
  switch (key[0]) {
    case kTagOid:
      if (payload.size() != 12) throw bad_value("malformed ObjectId _id key");
      v.value_type = BSON_TYPE_OID;
      bson_oid_init_from_data(&v.value.v_oid, reinterpret_cast<const uint8_t*>(payload.data()));
      return Value(v);
    case kTagString:
      return Value::from_utf8(payload);
    case kTagInt:
      if (payload.size() != 8) throw bad_value("malformed integer _id key");
      return Value::from_int64(decode_int(payload));
    default:
      throw bad_value("unknown _id key tag");
  }
}

}  // namespace chimera
