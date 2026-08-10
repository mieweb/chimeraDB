#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/id.h"

using namespace chimera;

static Value id_of(const char* extjson) {
  static Bson holder;
  holder = from_extjson(extjson);
  auto v = path_get(holder.get(), {"_id"});
  REQUIRE(v.has_value());
  return *v;
}

TEST_CASE("every supported _id type encodes and decodes back to itself") {
  const char* ids[] = {
      R"({"_id": {"$oid": "0123456789abcdef01234567"}})",
      R"({"_id": "aBcDeFgHiJkLmNoPq"})",   // Meteor's default random string id
      R"({"_id": ""})",
      R"({"_id": {"$numberLong": "-9223372036854775808"}})",
      R"({"_id": {"$numberLong": "9223372036854775807"}})",
  };
  for (const char* json : ids) {
    Value original = id_of(json);
    Value decoded = decode_id(encode_id(original.get()));
    CHECK(value_equal(original.get(), decoded.get()));
  }
}

TEST_CASE("int32 and int64 ids share one key, so duplicates are detected") {
  Value as_int32 = id_of(R"({"_id": {"$numberInt": "42"}})");
  Value as_int64 = id_of(R"({"_id": {"$numberLong": "42"}})");
  CHECK(encode_id(as_int32.get()) == encode_id(as_int64.get()));
  CHECK(decode_id(encode_id(as_int32.get())).type() == BSON_TYPE_INT64);
}

TEST_CASE("different id types never collide") {
  Value oid = id_of(R"({"_id": {"$oid": "0123456789abcdef01234567"}})");
  Value str = id_of(R"({"_id": "0123456789abcdef01234567"})");
  Value num = id_of(R"({"_id": {"$numberLong": "1"}})");
  CHECK(encode_id(oid.get()) != encode_id(str.get()));
  CHECK(encode_id(str.get()) != encode_id(num.get()));
}

TEST_CASE("integer key order is numeric order, so the InnoDB PK sorts correctly") {
  std::vector<int64_t> numbers = {-9223372036854775807LL - 1, -2, -1, 0, 1, 2, 1000,
                                  9223372036854775807LL};
  std::vector<std::string> keys;
  for (int64_t n : numbers) keys.push_back(encode_id(Value::from_int64(n).get()));
  std::vector<std::string> sorted = keys;
  std::shuffle(sorted.begin(), sorted.end(), std::mt19937(1));
  std::sort(sorted.begin(), sorted.end());
  CHECK(sorted == keys);
}

TEST_CASE("unsupported id types are rejected, never mangled") {
  Value floating = id_of(R"({"_id": {"$numberDouble": "1.5"}})");
  CHECK_THROWS_AS(encode_id(floating.get()), TranslatorError);
  CHECK_THROWS_AS(decode_id(""), TranslatorError);
  CHECK_THROWS_AS(decode_id(std::string(1, '\x99')), TranslatorError);
}
