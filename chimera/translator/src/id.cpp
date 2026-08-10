#include "chimera/id.h"

#include <cmath>
#include <cstring>

#include "chimera/error.h"

namespace chimera {
namespace {

constexpr char kTagDouble = static_cast<char>(BSON_TYPE_DOUBLE);  // 0x01
constexpr char kTagString = static_cast<char>(BSON_TYPE_UTF8);   // 0x02
constexpr char kTagOid = static_cast<char>(BSON_TYPE_OID);       // 0x07
constexpr char kTagInt = static_cast<char>(BSON_TYPE_INT64);     // 0x12

std::string big_endian(uint64_t bits) {
  std::string out(8, '\0');
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>((bits >> (56 - 8 * i)) & 0xFF);
  return out;
}

uint64_t from_big_endian(const std::string& bytes) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits = (bits << 8) | static_cast<uint8_t>(bytes[i]);
  return bits;
}

// Flipping the sign bit makes two's-complement integers sort correctly as
// unsigned big-endian bytes, so InnoDB's key order matches numeric order.
std::string encode_int(int64_t n) {
  return big_endian(static_cast<uint64_t>(n) ^ (1ULL << 63));
}

int64_t decode_int(const std::string& bytes) {
  return static_cast<int64_t>(from_big_endian(bytes) ^ (1ULL << 63));
}

// IEEE-754 already sorts correctly as unsigned bytes for positive values once
// the sign bit is set; negatives sort in reverse, so they are inverted whole.
std::string encode_double(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof bits);
  return big_endian((bits >> 63) ? ~bits : (bits | (1ULL << 63)));
}

double decode_double(const std::string& bytes) {
  uint64_t bits = from_big_endian(bytes);
  bits = (bits >> 63) ? (bits & ~(1ULL << 63)) : ~bits;
  double d;
  std::memcpy(&d, &bits, sizeof d);
  return d;
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
    case BSON_TYPE_DOUBLE: {
      // Every unadorned number from a JavaScript client is a double, so 1 and
      // NumberLong(1) must land on the same key or `{_id: 1}` would insert
      // twice. Only a value with a fractional part needs its own encoding.
      const double d = id.value.v_double;
      if (std::isfinite(d) && d == std::trunc(d) && d >= -9223372036854775808.0 &&
          d < 9223372036854775808.0) {
        return std::string(1, kTagInt) + encode_int(static_cast<int64_t>(d));
      }
      return std::string(1, kTagDouble) + encode_double(d);
    }
    default:
      throw type_mismatch("unsupported _id type — expected ObjectId, string, or number");
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
    case kTagDouble:
      if (payload.size() != 8) throw bad_value("malformed double _id key");
      return Value::from_double(decode_double(payload));
    default:
      throw bad_value("unknown _id key tag");
  }
}

}  // namespace chimera
