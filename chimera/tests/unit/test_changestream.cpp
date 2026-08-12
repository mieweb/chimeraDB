// The decidable half of $changeStream: token round-trips and stage-option
// parsing. Both are pure, so the whole matrix is testable with no server —
// which is the point of putting them in the translator (M7.1's precedent).
#include <doctest/doctest.h>

#include <string>

#include "chimera/bson.h"
#include "chimera/changestream.h"
#include "chimera/codec.h"
#include "chimera/error.h"

using namespace chimera;

namespace {

ChangeStreamOptions parse(const std::string& extjson) {
  Bson stage = from_extjson(extjson);
  return parse_change_stream(stage.get());
}

uint64_t decode(const std::string& extjson) {
  Bson token = from_extjson(extjson);
  return decode_resume_token(token.get());
}

// The code a driver sees, which is the part that decides whether it retries.
int code_of(const std::string& extjson) {
  try {
    parse(extjson);
  } catch (const TranslatorError& e) {
    return e.code();
  }
  return 0;
}

}  // namespace

TEST_CASE("a resume token is a fixed-width hex rendering of the oplog sequence") {
  CHECK(encode_resume_token(0) == "0000000000000000");
  CHECK(encode_resume_token(1) == "0000000000000001");
  CHECK(encode_resume_token(255) == "00000000000000ff");

  // Fixed width is what makes the encoding checkable rather than guessable, and
  // it must hold at the top of the range too.
  CHECK(encode_resume_token(UINT64_MAX) == "ffffffffffffffff");
  CHECK(encode_resume_token(UINT64_MAX).size() == 16);
}

TEST_CASE("every sequence survives the round trip") {
  for (const uint64_t seq : {uint64_t{0}, uint64_t{1}, uint64_t{42}, uint64_t{1} << 32,
                             uint64_t{9007199254740993}, UINT64_MAX}) {
    const std::string doc = R"({"_data": ")" + encode_resume_token(seq) + R"("})";
    CHECK(decode(doc) == seq);
  }
}

TEST_CASE("a token that is not ours is refused rather than guessed at") {
  // Retrying a malformed token forever helps nobody, so these are BadValue (a
  // driver treats them as non-resumable) and not a resumable error.
  CHECK_THROWS_AS(decode(R"({"nope": "0000000000000001"})"), TranslatorError);
  CHECK_THROWS_AS(decode(R"({"_data": 1})"), TranslatorError);
  CHECK_THROWS_AS(decode(R"({"_data": "abc"})"), TranslatorError);              // too short
  CHECK_THROWS_AS(decode(R"({"_data": "00000000000000001"})"), TranslatorError);  // too long
  CHECK_THROWS_AS(decode(R"({"_data": "zzzzzzzzzzzzzzzz"})"), TranslatorError);   // not hex
  // A real MongoDB token is uppercase-hex keystring of a different width; ours
  // must not silently accept one and decode it to nonsense.
  CHECK_THROWS_AS(decode(R"({"_data": "826553A0C1000000012B02"})"), TranslatorError);
  CHECK_THROWS_AS(decode(R"({"_data": "000000000000000A"})"), TranslatorError);
}

TEST_CASE("an empty stage starts from the head") {
  const ChangeStreamOptions opts = parse("{}");
  CHECK(opts.start == ChangeStreamStart::kHead);
  CHECK(opts.after_seq == 0);
}

TEST_CASE("Meteor's exact stage body parses") {
  // Transcribed from shared_change_stream.js — if this stops parsing, Meteor
  // stops being reactive.
  const ChangeStreamOptions opts = parse(R"({
    "fullDocument": "updateLookup",
    "fullDocumentBeforeChange": "whenAvailable"
  })");
  CHECK(opts.start == ChangeStreamStart::kHead);
}

TEST_CASE("every fullDocument mode is satisfiable, because we always have the post-image") {
  for (const char* mode : {"default", "updateLookup", "whenAvailable", "required"}) {
    CHECK_NOTHROW(parse(std::string(R"({"fullDocument": ")") + mode + R"("})"));
  }
  CHECK(code_of(R"({"fullDocument": "sometimes"})") == 2);
  CHECK(code_of(R"({"fullDocument": 1})") == 14);
}

TEST_CASE("pre-images are declined loudly, not promised and then omitted") {
  CHECK_NOTHROW(parse(R"({"fullDocumentBeforeChange": "off"})"));
  CHECK_NOTHROW(parse(R"({"fullDocumentBeforeChange": "whenAvailable"})"));
  // 238 NotImplemented: we store no pre-images, so 'required' cannot be served.
  CHECK(code_of(R"({"fullDocumentBeforeChange": "required"})") == 238);
}

TEST_CASE("a resume origin sets where the stream begins") {
  const ChangeStreamOptions after = parse(R"({"startAfter": {"_data": "000000000000002a"}})");
  CHECK(after.start == ChangeStreamStart::kToken);
  CHECK(after.after_seq == 42);

  const ChangeStreamOptions resume = parse(R"({"resumeAfter": {"_data": "0000000000000007"}})");
  CHECK(resume.start == ChangeStreamStart::kToken);
  CHECK(resume.after_seq == 7);

  const ChangeStreamOptions at =
      parse(R"({"startAtOperationTime": {"$timestamp": {"t": 1700000000, "i": 3}}})");
  CHECK(at.start == ChangeStreamStart::kOperationTime);
  CHECK(at.ts_t == 1700000000);
  CHECK(at.ts_i == 3);
}

TEST_CASE("the three resume origins are mutually exclusive, in every pairing") {
  const char* origins[] = {R"("resumeAfter": {"_data": "0000000000000001"})",
                           R"("startAfter": {"_data": "0000000000000002"})",
                           R"("startAtOperationTime": {"$timestamp": {"t": 5, "i": 1}})"};
  for (const char* a : origins) {
    for (const char* b : origins) {
      // Same key twice is a duplicate field, not a conflict; only distinct
      // pairs are what MongoDB's 40674 is about.
      if (a == b) continue;
      CHECK(code_of("{" + std::string(a) + ", " + b + "}") == 40674);
    }
  }
}

TEST_CASE("scope switches are accepted only in the shape that asks for nothing") {
  CHECK_NOTHROW(parse(R"({"allChangesForCluster": false})"));
  CHECK_NOTHROW(parse(R"({"showExpandedEvents": false})"));
  CHECK(code_of(R"({"allChangesForCluster": true})") == 238);
  CHECK(code_of(R"({"showExpandedEvents": true})") == 238);
}

TEST_CASE("an option we have never heard of is refused, never ignored") {
  // A stream that silently drops an option looks like it is working while it
  // loses events — the one failure this whole design refuses to have.
  CHECK(code_of(R"({"someFutureOption": 1})") == 238);
}
