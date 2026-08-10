#include <doctest/doctest.h>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/update.h"

using namespace chimera;

static std::string updated(const char* doc, const char* update) {
  Bson d = from_extjson(doc);
  Bson u = from_extjson(update);
  return to_extjson(apply_update(d.get(), u.get()).doc.get());
}

TEST_CASE("$set writes scalars, creates missing paths, and preserves field order") {
  CHECK(updated(R"({"a": 1})", R"({"$set": {"a": 2}})") ==
        R"({ "a" : { "$numberInt" : "2" } })");
  CHECK(updated(R"({"a": 1})", R"({"$set": {"b": "x"}})") ==
        R"({ "a" : { "$numberInt" : "1" }, "b" : "x" })");
  CHECK(updated(R"({})", R"({"$set": {"a.b.c": true}})") ==
        R"({ "a" : { "b" : { "c" : true } } })");
  CHECK(updated(R"({"a": {"b": 1, "c": 2}})", R"({"$set": {"a.b": 9}})") ==
        R"({ "a" : { "b" : { "$numberInt" : "9" }, "c" : { "$numberInt" : "2" } } })");
}

TEST_CASE("$unset removes a field and is a no-op when it is already absent") {
  CHECK(updated(R"({"a": 1, "b": 2})", R"({"$unset": {"a": ""}})") ==
        R"({ "b" : { "$numberInt" : "2" } })");
  CHECK(updated(R"({"a": {"b": 1, "c": 2}})", R"({"$unset": {"a.b": ""}})") ==
        R"({ "a" : { "c" : { "$numberInt" : "2" } } })");
  CHECK(updated(R"({"a": 1})", R"({"$unset": {"zzz": ""}})") ==
        R"({ "a" : { "$numberInt" : "1" } })");
}

TEST_CASE("$inc treats a missing field as zero and keeps integers integral") {
  CHECK(updated(R"({"n": 1})", R"({"$inc": {"n": 2}})") == R"({ "n" : { "$numberLong" : "3" } })");
  CHECK(updated(R"({})", R"({"$inc": {"n": 5}})") == R"({ "n" : { "$numberLong" : "5" } })");
  CHECK(updated(R"({"n": 1})", R"({"$inc": {"n": -3}})") ==
        R"({ "n" : { "$numberLong" : "-2" } })");
  CHECK(updated(R"({"n": 1})", R"({"$inc": {"n": 0.5}})") ==
        R"({ "n" : { "$numberDouble" : "1.5" } })");
  CHECK_THROWS_AS(updated(R"({"n": "x"})", R"({"$inc": {"n": 1}})"), TranslatorError);
}

TEST_CASE("$push appends, creating the array if needed, and honors $each") {
  CHECK(updated(R"({})", R"({"$push": {"t": "a"}})") == R"({ "t" : [ "a" ] })");
  CHECK(updated(R"({"t": ["a"]})", R"({"$push": {"t": "b"}})") == R"({ "t" : [ "a", "b" ] })");
  CHECK(updated(R"({"t": ["a"]})", R"({"$push": {"t": {"$each": ["b", "c"]}}})") ==
        R"({ "t" : [ "a", "b", "c" ] })");
  CHECK_THROWS_AS(updated(R"({"t": 1})", R"({"$push": {"t": "a"}})"), TranslatorError);
}

TEST_CASE("$addToSet skips values already present, comparing numbers numerically") {
  CHECK(updated(R"({"t": ["a"]})", R"({"$addToSet": {"t": "a"}})") == R"({ "t" : [ "a" ] })");
  CHECK(updated(R"({"t": ["a"]})", R"({"$addToSet": {"t": "b"}})") == R"({ "t" : [ "a", "b" ] })");
  CHECK(updated(R"({"t": [{"$numberInt": "1"}]})",
                R"({"$addToSet": {"t": {"$numberLong": "1"}}})") ==
        R"({ "t" : [ { "$numberInt" : "1" } ] })");
}

TEST_CASE("$pull removes every equal element; $pop takes an end") {
  CHECK(updated(R"({"t": ["a", "b", "a"]})", R"({"$pull": {"t": "a"}})") == R"({ "t" : [ "b" ] })");
  CHECK(updated(R"({"t": ["a", "b"]})", R"({"$pop": {"t": 1}})") == R"({ "t" : [ "a" ] })");
  CHECK(updated(R"({"t": ["a", "b"]})", R"({"$pop": {"t": -1}})") == R"({ "t" : [ "b" ] })");
  CHECK(updated(R"({"t": []})", R"({"$pop": {"t": 1}})") == R"({ "t" : [  ] })");
}

TEST_CASE("a replacement document replaces everything but keeps _id") {
  CHECK(updated(R"({"_id": "u1", "a": 1})", R"({"b": 2})") ==
        R"({ "b" : { "$numberInt" : "2" }, "_id" : "u1" })");
}

TEST_CASE("operators report which paths they touched, for the oplog writer") {
  Bson doc = from_extjson(R"({"a": 1})");
  Bson update = from_extjson(R"({"$set": {"a": 2, "b.c": 3}, "$inc": {"n": 1}})");
  auto result = apply_update(doc.get(), update.get());
  CHECK(result.changed == std::vector<std::string>{"a", "b.c", "n"});
}

TEST_CASE("unsupported and malformed updates fail fast") {
  CHECK_THROWS_AS(updated(R"({"a": 1})", R"({"$rename": {"a": "b"}})"), TranslatorError);
  CHECK_THROWS_AS(updated(R"({"a": [1]})", R"({"$set": {"a.$": 2}})"), TranslatorError);
  CHECK_THROWS_AS(updated(R"({"a": [1]})", R"({"$pull": {"a": {"$gt": 0}}})"), TranslatorError);
  CHECK_THROWS_AS(updated(R"({"a": 1})", R"({"$set": {"a": 1}, "b": 2})"), TranslatorError);
}
