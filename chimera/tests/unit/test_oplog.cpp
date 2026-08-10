// The oplog's own query shapes. Meteor issues exactly these three against
// local.oplog.rs, and they are the only place a Timestamp reaches the filter
// compiler — so they get tested on their own rather than buried in test_filter.
#include <doctest/doctest.h>

#include <string>

#include "chimera/bson.h"
#include "chimera/codec.h"
#include "chimera/filter.h"

using namespace chimera;

namespace {

SqlFilter compile(const std::string& extjson) {
  Bson doc = from_extjson(extjson);
  return compile_filter(doc.get(), "doc");
}

int64_t param_int(const Param& p) { return std::get<int64_t>(p.value); }

}  // namespace

TEST_CASE("a Timestamp compares as one packed 64-bit ordinal") {
  auto f = compile(R"({"ts": {"$gt": {"$timestamp": {"t": 1700000000, "i": 7}}}})");

  // (t, i) is a lexicographic pair, so it must compare as t * 2^32 + i rather
  // than as two independent numbers or as an inexact DOUBLE.
  CHECK(f.sql.find("4294967296") != std::string::npos);
  CHECK(f.sql.find(R"($."ts"."$timestamp".t)") != std::string::npos);
  CHECK(f.sql.find(R"($."ts"."$timestamp".i)") != std::string::npos);
  REQUIRE(f.params.size() == 1);
  CHECK(param_int(f.params[0]) == (int64_t{1700000000} << 32) + 7);
}

TEST_CASE("the ordinal survives the increment rolling over into the next second") {
  auto low = compile(R"({"ts": {"$gt": {"$timestamp": {"t": 100, "i": 4294967295}}}})");
  auto high = compile(R"({"ts": {"$gt": {"$timestamp": {"t": 101, "i": 0}}}})");
  CHECK(param_int(low.params[0]) < param_int(high.params[0]));
}

TEST_CASE("Meteor's tail: everything after a point, on a set of namespaces") {
  auto f = compile(R"({
    "ts": {"$gt": {"$timestamp": {"t": 1700000000, "i": 1}}},
    "ns": {"$in": ["meteor.todos", "meteor.lists"]}
  })");

  CHECK(f.sql.find("4294967296") != std::string::npos);
  CHECK(f.sql.find(" AND ") != std::string::npos);
  CHECK(f.sql.find("IN (?, ?)") != std::string::npos);
  REQUIRE(f.params.size() == 3);
}

TEST_CASE("Meteor also watches a single collection with an $or over ops") {
  auto f = compile(R"({
    "ns": "meteor.todos",
    "$or": [{"op": "i"}, {"op": "u"}, {"op": "d"}]
  })");

  CHECK(f.sql.find(" OR ") != std::string::npos);
  // Each equality binds twice — once as a scalar and once as the JSON form that
  // matches an array element — so four comparisons carry eight parameters.
  REQUIRE(f.params.size() == 8);
}

TEST_CASE("Timestamps round-trip through canonical extJSON unchanged") {
  const std::string entry =
      R"({ "ts" : { "$timestamp" : { "t" : 1700000000, "i" : 7 } }, "op" : "i" })";
  Bson doc = from_extjson(entry);
  CHECK(to_extjson(doc.get()) == entry);
}
