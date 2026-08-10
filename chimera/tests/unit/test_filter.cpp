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

// What the compiler emits to read one scalar. Canonical extJSON hides every
// non-string scalar inside a type wrapper, so a read is a COALESCE over the
// encodings the value could have; spelling it out once keeps the expectations
// below readable. The wrapper list itself is pinned literally by the test
// immediately following.
static std::string scalar(const std::string& path) {
  static const char* const kWrappers[] = {"",
                                          R"(."$numberInt")",
                                          R"(."$numberLong")",
                                          R"(."$numberDouble")",
                                          R"(."$date"."$numberLong")",
                                          R"(."$oid")"};
  std::string out = "COALESCE(";
  for (size_t i = 0; i < sizeof kWrappers / sizeof kWrappers[0]; ++i) {
    if (i > 0) out += ",";
    out += "JSON_VALUE(doc,'" + path + kWrappers[i] + "')";
  }
  return out + ")";
}

static std::string numeric(const std::string& path) {
  return "CAST(" + scalar(path) + " AS DOUBLE)";
}

TEST_CASE("reading a field looks through every canonical extJSON type wrapper") {
  CHECK(compile(R"({"a": {"$gte": "x"}})").sql ==
        R"(COALESCE(JSON_VALUE(doc,'$."a"'),JSON_VALUE(doc,'$."a"."$numberInt"'),)"
        R"(JSON_VALUE(doc,'$."a"."$numberLong"'),JSON_VALUE(doc,'$."a"."$numberDouble"'),)"
        R"(JSON_VALUE(doc,'$."a"."$date"."$numberLong"'),JSON_VALUE(doc,'$."a"."$oid"')) >= ?)");
}

TEST_CASE("an empty filter matches everything") {
  auto f = compile("{}");
  CHECK(f.sql == "1");
  CHECK(f.params.empty());
}

TEST_CASE("implicit equality also matches an array containing the value") {
  auto f = compile(R"({"name": "Doug"})");
  CHECK(f.sql == "(" + scalar(R"($."name")") +
                     R"( = ? OR JSON_CONTAINS(JSON_EXTRACT(doc,'$."name"'), ?)))");
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
  CHECK(compile(R"({"age": {"$gt": 30}})").sql == numeric(R"($."age")") + " > ?");
  CHECK(compile(R"({"name": {"$gte": "m"}})").sql == scalar(R"($."name")") + " >= ?");
}

TEST_CASE("dates compare as epoch milliseconds, so a $gt on a date works") {
  auto f = compile(R"({"createdAt": {"$gt": {"$date": {"$numberLong": "1786694400000"}}}})");
  CHECK(f.sql == numeric(R"($."createdAt")") + " > ?");
  REQUIRE(f.params.size() == 1);
  CHECK(std::get<int64_t>(f.params[0].value) == 1786694400000LL);
}

TEST_CASE("an ObjectId compares against its hex form, which is how it is stored") {
  auto f = compile(R"({"_id": {"$oid": "6a79875bbf1d558e7e0690a0"}})");
  REQUIRE(f.params.size() == 2);
  CHECK(param_string(f.params[0]) == "6a79875bbf1d558e7e0690a0");
}

TEST_CASE("dotted and extJSON-typed paths become quoted JSON paths") {
  CHECK(compile(R"({"a.b": {"$lt": 1}})").sql == numeric(R"($."a"."b")") + " < ?");
  CHECK(compile(R"({"createdAt.$date.$numberLong": {"$lte": 1}})").sql ==
        numeric(R"($."createdAt"."$date"."$numberLong")") + " <= ?");
}

TEST_CASE("$in and $nin bind every member; empty lists short-circuit") {
  auto in = compile(R"({"k": {"$in": ["a", "b"]}})");
  CHECK(in.sql == scalar(R"($."k")") + " IN (?, ?)");
  REQUIRE(in.params.size() == 2);
  CHECK(param_string(in.params[1]) == "b");
  CHECK(compile(R"({"k": {"$in": []}})").sql == "0");
  CHECK(compile(R"({"k": {"$nin": []}})").sql == "1");
}

TEST_CASE("$and, $or and $not compose without redundant parentheses") {
  CHECK(compile(R"({"$or": [{"a": {"$gt": 1}}, {"b": {"$lt": 2}}]})").sql ==
        "(" + numeric(R"($."a")") + " > ? OR " + numeric(R"($."b")") + " < ?)");
  CHECK(compile(R"({"$and": [{"a": {"$gte": 1}}]})").sql == numeric(R"($."a")") + " >= ?");
  CHECK(compile(R"({"a": {"$not": {"$gt": 1}}})").sql ==
        "NOT (" + numeric(R"($."a")") + " > ?)");
  CHECK(compile(R"({"a": {"$gt": 1}, "b": {"$lt": 2}})").sql ==
        "(" + numeric(R"($."a")") + " > ? AND " + numeric(R"($."b")") + " < ?)");
}

TEST_CASE("$exists maps to JSON_EXISTS, in both directions") {
  CHECK(compile(R"({"a": {"$exists": true}})").sql == R"(JSON_EXISTS(doc,'$."a"'))");
  CHECK(compile(R"({"a": {"$exists": false}})").sql == R"(NOT JSON_EXISTS(doc,'$."a"'))");
}

TEST_CASE("$ne negates the equality form, so arrays are handled consistently") {
  CHECK(compile(R"({"a": {"$ne": null}})").sql == "NOT (" + scalar(R"($."a")") + " IS NULL)");
}

TEST_CASE("a regex matches rather than compares, and the pattern is bound") {
  // libbson decodes {"$regex": ...} into a BSON regex, which is also what the
  // drivers send for {name: /^Do/} — both arrive here as a bare regex value.
  auto f = compile(R"({"name": {"$regex": "^Do"}})");
  CHECK(f.sql == scalar(R"($."name")") + " REGEXP ?");
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
    // An operator we simply do not recognize reports what MongoDB reports, so a
    // driver's error handling behaves identically against either server.
    compile(R"({"a": {"$mod": [2, 0]}})");
  } catch (const TranslatorError& e) {
    CHECK(e.code_name() == "BadValue");
  }
  try {
    // One we do recognize but have not built yet stays honest about that.
    compile(R"({"a": {"$elemMatch": {"$gt": 1}}})");
  } catch (const TranslatorError& e) {
    CHECK(e.code_name() == "NotImplemented");
  }
}
