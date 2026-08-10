#include <doctest/doctest.h>

#include "chimera/codec.h"
#include "chimera/error.h"

using namespace chimera;

// D5: every BSON type Meteor and the drivers care about must survive a trip
// through the JSON column and back unchanged.
static void round_trips(const char* canonical) {
  Bson doc = from_extjson(canonical);
  CHECK(to_extjson(doc.get()) == canonical);
}

TEST_CASE("canonical extJSON round-trips every BSON type we store") {
  round_trips(R"({ "_id" : { "$oid" : "0123456789abcdef01234567" } })");
  round_trips(R"({ "at" : { "$date" : { "$numberLong" : "1786694400000" } } })");
  round_trips(R"({ "ts" : { "$timestamp" : { "t" : 1786694400, "i" : 3 } } })");
  round_trips(R"({ "price" : { "$numberDecimal" : "12.35" } })");
  round_trips(R"({ "blob" : { "$binary" : { "base64" : "ZmZm", "subType" : "00" } } })");
  round_trips(R"({ "n" : { "$numberInt" : "7" }, "big" : { "$numberLong" : "9007199254740993" } })");
  round_trips(R"({ "d" : { "$numberDouble" : "1.5" } })");
  round_trips(R"({ "tags" : [ "a", { "$numberInt" : "1" }, { "nested" : [ true ] } ] })");
  round_trips(R"({ "missing" : null, "flag" : false })");
}

TEST_CASE("relaxed extJSON input is normalized to canonical on the way in") {
  Bson doc = from_extjson(R"({"n": 7, "s": "x"})");
  CHECK(to_extjson(doc.get()) == R"({ "n" : { "$numberInt" : "7" }, "s" : "x" })");
}

TEST_CASE("malformed JSON fails loudly with a parse error") {
  CHECK_THROWS_AS(from_extjson("{not json"), TranslatorError);
  try {
    from_extjson("{not json");
  } catch (const TranslatorError& e) {
    CHECK(e.code() == 9);
    CHECK(e.code_name() == "FailedToParse");
  }
}
