#include <doctest/doctest.h>

#include <vector>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/project.h"
#include "chimera/sort.h"

using namespace chimera;

static std::vector<Bson> docs(const std::vector<const char*>& jsons) {
  std::vector<Bson> out;
  for (const char* json : jsons) out.push_back(from_extjson(json));
  return out;
}

static std::string id_order(const std::vector<Bson>& sorted) {
  std::string out;
  for (const auto& doc : sorted) {
    auto id = path_get(doc.get(), {"_id"});
    out += std::string(id->get().value.v_utf8.str, id->get().value.v_utf8.len);
  }
  return out;
}

TEST_CASE("sorting follows the BSON type order, not just the values") {
  // A number always sorts before a string, however large the number.
  auto values = docs({R"({"_id":"s","v":"1"})", R"({"_id":"n","v":999999})",
                      R"({"_id":"z","v":null})", R"({"_id":"b","v":true})"});
  Bson spec = from_extjson(R"({"v": 1})");
  sort_documents(values, spec.get());
  CHECK(id_order(values) == "znsb");
}

TEST_CASE("the three numeric types are one rank, so 1 and NumberLong(1) tie") {
  auto values = docs({R"({"_id":"a","v":{"$numberLong":"2"}})", R"({"_id":"b","v":1.5})",
                      R"({"_id":"c","v":{"$numberInt":"1"}})"});
  Bson spec = from_extjson(R"({"v": 1})");
  sort_documents(values, spec.get());
  CHECK(id_order(values) == "cba");
}

TEST_CASE("descending reverses, and a missing field sorts as null") {
  auto values = docs({R"({"_id":"a","v":1})", R"({"_id":"b"})", R"({"_id":"c","v":5})"});
  Bson spec = from_extjson(R"({"v": -1})");
  sort_documents(values, spec.get());
  CHECK(id_order(values) == "cab");
}

TEST_CASE("later sort keys break ties, and equal documents keep their order") {
  auto values = docs({R"({"_id":"a","g":1,"n":2})", R"({"_id":"b","g":1,"n":1})",
                      R"({"_id":"c","g":0,"n":9})", R"({"_id":"d","g":1,"n":1})"});
  Bson spec = from_extjson(R"({"g": 1, "n": 1})");
  sort_documents(values, spec.get());
  CHECK(id_order(values) == "cbda");
}

TEST_CASE("dotted sort keys reach into subdocuments") {
  auto values = docs({R"({"_id":"a","p":{"n":2}})", R"({"_id":"b","p":{"n":1}})"});
  Bson spec = from_extjson(R"({"p.n": 1})");
  sort_documents(values, spec.get());
  CHECK(id_order(values) == "ba");
}

TEST_CASE("a sort direction other than 1 or -1 is rejected") {
  auto values = docs({R"({"_id":"a"})"});
  Bson spec = from_extjson(R"({"v": 2})");
  CHECK_THROWS_AS(sort_documents(values, spec.get()), TranslatorError);
}

TEST_CASE("an inclusion projection keeps _id unless it is switched off") {
  Bson doc = from_extjson(R"({"_id":"u1","name":"Doug","age":30,"city":"Fort Wayne"})");
  Bson spec = from_extjson(R"({"name": 1})");
  CHECK(to_extjson(project(doc.get(), spec.get()).get()) ==
        R"({ "_id" : "u1", "name" : "Doug" })");

  Bson without_id = from_extjson(R"({"name": 1, "_id": 0})");
  CHECK(to_extjson(project(doc.get(), without_id.get()).get()) == R"({ "name" : "Doug" })");
}

TEST_CASE("an exclusion projection keeps everything else") {
  Bson doc = from_extjson(R"({"_id":"u1","name":"Doug","age":30})");
  Bson spec = from_extjson(R"({"age": 0})");
  CHECK(to_extjson(project(doc.get(), spec.get()).get()) ==
        R"({ "_id" : "u1", "name" : "Doug" })");
}

TEST_CASE("projections reach into subdocuments") {
  Bson doc = from_extjson(R"({"_id":"u1","p":{"a":1,"b":2}})");
  Bson spec = from_extjson(R"({"p.a": 1})");
  CHECK(to_extjson(project(doc.get(), spec.get()).get()) ==
        R"({ "_id" : "u1", "p" : { "a" : { "$numberInt" : "1" } } })");
}

TEST_CASE("mixing inclusion and exclusion is an error, but excluding _id is not") {
  Bson doc = from_extjson(R"({"_id":"u1","a":1,"b":2})");
  Bson mixed = from_extjson(R"({"a": 1, "b": 0})");
  CHECK_THROWS_AS(project(doc.get(), mixed.get()), TranslatorError);

  Bson empty = from_extjson("{}");
  CHECK(to_extjson(project(doc.get(), empty.get()).get()) == to_extjson(doc.get()));
}
