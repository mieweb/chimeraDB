#include <doctest/doctest.h>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/filter.h"

using namespace chimera;

static SqlFilter compile(const char* json) {
  Bson filter = from_extjson(json);
  return compile_filter(filter.get());
}

static std::string param_string(const Param& p) { return std::get<std::string>(p.value); }

TEST_CASE("an empty filter matches everything") {
  auto f = compile("{}");
  CHECK(f.sql == "1");
  CHECK(f.params.empty());
}

TEST_CASE("implicit equality also matches an array containing the value") {
  auto f = compile(R"({"name": "Doug"})");
  CHECK(f.sql ==
        R"((JSON_VALUE(doc,'$."name"') = ? OR JSON_CONTAINS(JSON_EXTRACT(doc,'$."name"'), ?)))");
  REQUIRE(f.params.size() == 2);
  CHECK(param_string(f.params[0]) == "Doug");
  // The containment candidate must be canonical extJSON, matching what is stored.
  CHECK(param_string(f.params[1]) == "\"Doug\"");
}

TEST_CASE("the containment candidate uses the stored canonical encoding for numbers") {
  auto f = compile(R"({"n": {"$numberInt": "1"}})");
  REQUIRE(f.params.size() == 2);
  CHECK(std::get<int64_t>(f.params[0].value) == 1);
  CHECK(param_string(f.params[1]) == R"({ "$numberInt" : "1" })");
}

TEST_CASE("numeric comparisons cast, string comparisons do not") {
  CHECK(compile(R"({"age": {"$gt": 30}})").sql ==
        R"(CAST(JSON_VALUE(doc,'$."age"') AS DOUBLE) > ?)");
  CHECK(compile(R"({"name": {"$gte": "m"}})").sql == R"(JSON_VALUE(doc,'$."name"') >= ?)");
}

TEST_CASE("dotted and extJSON-typed paths become quoted JSON paths") {
  CHECK(compile(R"({"a.b": {"$lt": 1}})").sql ==
        R"(CAST(JSON_VALUE(doc,'$."a"."b"') AS DOUBLE) < ?)");
  CHECK(compile(R"({"createdAt.$date.$numberLong": {"$lte": 1}})").sql ==
        R"(CAST(JSON_VALUE(doc,'$."createdAt"."$date"."$numberLong"') AS DOUBLE) <= ?)");
}

TEST_CASE("$in and $nin bind every member; empty lists short-circuit") {
  auto in = compile(R"({"k": {"$in": ["a", "b"]}})");
  CHECK(in.sql == R"(JSON_VALUE(doc,'$."k"') IN (?, ?))");
  REQUIRE(in.params.size() == 2);
  CHECK(param_string(in.params[1]) == "b");
  CHECK(compile(R"({"k": {"$in": []}})").sql == "0");
  CHECK(compile(R"({"k": {"$nin": []}})").sql == "1");
}

TEST_CASE("$and, $or and $not compose without redundant parentheses") {
  CHECK(compile(R"({"$or": [{"a": {"$gt": 1}}, {"b": {"$lt": 2}}]})").sql ==
        R"((CAST(JSON_VALUE(doc,'$."a"') AS DOUBLE) > ? OR CAST(JSON_VALUE(doc,'$."b"') AS DOUBLE) < ?))");
  CHECK(compile(R"({"$and": [{"a": {"$gte": 1}}]})").sql ==
        R"(CAST(JSON_VALUE(doc,'$."a"') AS DOUBLE) >= ?)");
  CHECK(compile(R"({"a": {"$not": {"$gt": 1}}})").sql ==
        R"(NOT (CAST(JSON_VALUE(doc,'$."a"') AS DOUBLE) > ?))");
  CHECK(compile(R"({"a": {"$gt": 1}, "b": {"$lt": 2}})").sql ==
        R"((CAST(JSON_VALUE(doc,'$."a"') AS DOUBLE) > ? AND CAST(JSON_VALUE(doc,'$."b"') AS DOUBLE) < ?))");
}

TEST_CASE("$exists maps to JSON_EXISTS, in both directions") {
  CHECK(compile(R"({"a": {"$exists": true}})").sql == R"(JSON_EXISTS(doc,'$."a"'))");
  CHECK(compile(R"({"a": {"$exists": false}})").sql == R"(NOT JSON_EXISTS(doc,'$."a"'))");
}

TEST_CASE("$ne negates the equality form, so arrays are handled consistently") {
  CHECK(compile(R"({"a": {"$ne": null}})").sql == R"(NOT (JSON_VALUE(doc,'$."a"') IS NULL))");
}

TEST_CASE("a regex matches rather than compares, and the pattern is bound") {
  // libbson decodes {"$regex": ...} into a BSON regex, which is also what the
  // drivers send for {name: /^Do/} — both arrive here as a bare regex value.
  auto f = compile(R"({"name": {"$regex": "^Do"}})");
  CHECK(f.sql == R"(JSON_VALUE(doc,'$."name"') REGEXP ?)");
  REQUIRE(f.params.size() == 1);
  CHECK(param_string(f.params[0]) == "^Do");
}

TEST_CASE("basic $elemMatch compiles to containment") {
  auto f = compile(R"({"items": {"$elemMatch": {"sku": "x"}}})");
  CHECK(f.sql == R"(JSON_CONTAINS(JSON_EXTRACT(doc,'$."items"'), ?))");
  REQUIRE(f.params.size() == 1);
  CHECK(param_string(f.params[0]) == R"({ "sku" : "x" })");
}

TEST_CASE("hostile values are bound, never interpolated") {
  auto f = compile(R"({"name": "'; DROP TABLE users; --"})");
  CHECK(f.sql.find("DROP TABLE") == std::string::npos);
  CHECK(param_string(f.params[0]) == "'; DROP TABLE users; --");
}

TEST_CASE("unsupported operators fail fast instead of returning wrong rows") {
  CHECK_THROWS_AS(compile(R"({"a": {"$mod": [2, 0]}})"), TranslatorError);
  CHECK_THROWS_AS(compile(R"({"$where": "true"})"), TranslatorError);
  CHECK_THROWS_AS(compile(R"({"a": {"$elemMatch": {"$gt": 1}}})"), TranslatorError);
  try {
    compile(R"({"a": {"$mod": [2, 0]}})");
  } catch (const TranslatorError& e) {
    CHECK(e.code_name() == "NotImplemented");
  }
}
