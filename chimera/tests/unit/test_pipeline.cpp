// The aggregation subset the README promises. Meteor reaches it through
// `countDocuments`, which is `[{$match: …}, {$group: {_id: 1, n: {$sum: 1}}}]` —
// so the split between what SQL answers and what this process answers is the
// part worth pinning down.
#include <doctest/doctest.h>

#include <string>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/pipeline.h"

using namespace chimera;

namespace {

Pipeline split(const std::string& extjson) {
  static Bson holder;
  holder = from_extjson("{\"p\": " + extjson + "}");
  bson_t view;
  bson_iter_t it;
  REQUIRE(bson_iter_init_find(&it, holder.get(), "p"));
  uint32_t length = 0;
  const uint8_t* data = nullptr;
  bson_iter_array(&it, &length, &data);
  REQUIRE(bson_init_static(&view, data, length));
  return split_pipeline(&view);
}

std::vector<Bson> docs(const std::vector<std::string>& extjson) {
  std::vector<Bson> out;
  for (const auto& text : extjson) out.push_back(from_extjson(text));
  return out;
}

std::string render(const std::vector<Bson>& documents) {
  std::string text;
  for (const auto& document : documents) {
    if (!text.empty()) text += ",";
    text += to_extjson(document.get());
  }
  return text;
}

}  // namespace

TEST_CASE("a leading $match is handed to SQL rather than run here") {
  Pipeline plan = split(R"([{"$match": {"done": false}}, {"$count": "n"}])");
  CHECK(to_extjson(plan.prefilter.get()) == R"({ "done" : false })");
  CHECK(plan.stages.size() == 1);
}

TEST_CASE("consecutive $match stages become one conjunction") {
  Pipeline plan = split(R"([{"$match": {"a": 1}}, {"$match": {"b": 2}}])");
  CHECK(to_extjson(plan.prefilter.get()) ==
        R"({ "$and" : [ { "a" : { "$numberInt" : "1" } }, { "b" : { "$numberInt" : "2" } } ] })");
  CHECK(plan.stages.empty());
}

TEST_CASE("a $match after a reshaping stage is refused, not approximated") {
  Pipeline plan = split(R"([{"$count": "n"}, {"$match": {"n": 1}}])");
  CHECK_THROWS_AS(run_stages({}, plan.stages), TranslatorError);
}

TEST_CASE("$count over an empty collection still answers") {
  Pipeline plan = split(R"([{"$count": "n"}])");
  CHECK(render(run_stages({}, plan.stages)) == R"({ "n" : { "$numberLong" : "0" } })");
}

TEST_CASE("Meteor's countDocuments shape") {
  Pipeline plan = split(R"([{"$match": {"done": false}}, {"$group": {"_id": 1, "n": {"$sum": 1}}}])");
  auto out = run_stages(docs({R"({"a": 1})", R"({"a": 2})"}), plan.stages);
  CHECK(render(out) ==
        R"({ "_id" : { "$numberInt" : "1" }, "n" : { "$numberDouble" : "2.0" } })");
}

TEST_CASE("$group by a field path sums another field") {
  Pipeline plan = split(R"([{"$group": {"_id": "$owner", "total": {"$sum": "$qty"}}}])");
  auto out = run_stages(docs({R"({"owner": "ann", "qty": 2})",
                              R"({"owner": "bob", "qty": 5})",
                              R"({"owner": "ann", "qty": 3})"}),
                        plan.stages);
  CHECK(render(out) == R"({ "_id" : "ann", "total" : { "$numberDouble" : "5.0" } },)"
                       R"({ "_id" : "bob", "total" : { "$numberDouble" : "5.0" } })");
}

TEST_CASE("$sort, $skip, $limit and $project compose in order") {
  Pipeline plan = split(
      R"([{"$sort": {"n": -1}}, {"$skip": 1}, {"$limit": 1}, {"$project": {"_id": 0, "n": 1}}])");
  auto out = run_stages(docs({R"({"_id": "a", "n": 1})",
                              R"({"_id": "b", "n": 3})",
                              R"({"_id": "c", "n": 2})"}),
                        plan.stages);
  CHECK(render(out) == R"({ "n" : { "$numberInt" : "2" } })");
}

TEST_CASE("an unsupported stage is an error, never a silent no-op") {
  Pipeline plan = split(R"([{"$lookup": {"from": "other"}}])");
  CHECK_THROWS_AS(run_stages({}, plan.stages), TranslatorError);
}
