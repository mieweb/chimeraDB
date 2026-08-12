#include "chimera/changestream.h"

#include <cstdio>
#include <cstring>
#include <string_view>

#include "chimera/error.h"

namespace chimera {

namespace {

// Where every refusal points, so a user who hits one finds the scope decision
// rather than just the word "unsupported".
const char kPlan[] = " (see changestream-plan.md)";

bool is_lower_hex(std::string_view s) {
  for (const char c : s) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) return false;
  }
  return true;
}

// A malformed token is reported as BadValue rather than as a resumable error:
// retrying the same bad token forever is the one behaviour that helps nobody.
[[noreturn]] void reject_token(const std::string& why) {
  throw bad_value("resume token is not valid: " + why + kPlan);
}

std::string_view utf8_of(const bson_iter_t& it, const char* field) {
  if (!BSON_ITER_HOLDS_UTF8(&it)) {
    throw type_mismatch(std::string("$changeStream option '") + field + "' must be a string");
  }
  uint32_t len = 0;
  const char* s = bson_iter_utf8(&it, &len);
  return {s, len};
}

// `false` is the default for both cluster-scope switches, so sending it
// explicitly is not a request for anything we lack.
void reject_if_requested(const bson_iter_t& it, const char* field) {
  if (!BSON_ITER_HOLDS_BOOL(&it)) {
    throw type_mismatch(std::string("$changeStream option '") + field + "' must be a boolean");
  }
  if (bson_iter_bool(&it)) {
    throw not_implemented(std::string("$changeStream option '") + field + "' is not supported" +
                          kPlan);
  }
}

void reject_second_origin(ChangeStreamStart current, const char* field) {
  if (current == ChangeStreamStart::kHead) return;
  throw change_stream_options_conflict(
      std::string("$changeStream options 'resumeAfter', 'startAfter' and 'startAtOperationTime' "
                  "are mutually exclusive; '") +
      field + "' was given as well");
}

}  // namespace

std::string encode_resume_token(uint64_t seq) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(seq));
  return std::string(buf, 16);
}

uint64_t decode_resume_token(const bson_t* token) {
  if (token == nullptr) reject_token("no token given");

  bson_iter_t it;
  if (!bson_iter_init_find(&it, token, "_data")) reject_token("no '_data' field");
  if (!BSON_ITER_HOLDS_UTF8(&it)) reject_token("'_data' is not a string");

  uint32_t len = 0;
  const char* data = bson_iter_utf8(&it, &len);
  const std::string_view hex(data, len);
  if (hex.size() != 16) reject_token("'_data' must be 16 hex digits");
  if (!is_lower_hex(hex)) reject_token("'_data' must be lowercase hex");

  return std::strtoull(std::string(hex).c_str(), nullptr, 16);
}

ChangeStreamOptions parse_change_stream(const bson_t* stage) {
  ChangeStreamOptions opts;
  if (stage == nullptr) return opts;

  bson_iter_t it;
  if (!bson_iter_init(&it, stage)) throw failed_to_parse("$changeStream stage is not a document");

  while (bson_iter_next(&it)) {
    const std::string_view key(bson_iter_key(&it));

    // Every mode is satisfiable: a 'u' oplog entry already carries the merged
    // post-image, so there is no lookup to decline.
    if (key == "fullDocument") {
      const std::string_view mode = utf8_of(it, "fullDocument");
      if (mode != "default" && mode != "updateLookup" && mode != "whenAvailable" &&
          mode != "required") {
        throw bad_value("unknown $changeStream fullDocument mode '" + std::string(mode) + "'");
      }
      continue;
    }

    // We store no pre-images. 'whenAvailable' is honest about that (the field is
    // simply absent from events); 'required' would be a promise we cannot keep.
    if (key == "fullDocumentBeforeChange") {
      const std::string_view mode = utf8_of(it, "fullDocumentBeforeChange");
      if (mode == "off" || mode == "whenAvailable") continue;
      if (mode == "required") {
        throw not_implemented(
            "$changeStream fullDocumentBeforeChange 'required': pre-images are not stored" +
            std::string(kPlan));
      }
      throw bad_value("unknown $changeStream fullDocumentBeforeChange mode '" + std::string(mode) +
                      "'");
    }

    if (key == "resumeAfter" || key == "startAfter") {
      const std::string field(key);
      reject_second_origin(opts.start, field.c_str());
      if (!BSON_ITER_HOLDS_DOCUMENT(&it)) reject_token("'" + field + "' must be a document");
      const uint8_t* buf = nullptr;
      uint32_t len = 0;
      bson_iter_document(&it, &len, &buf);
      bson_t token;
      if (!bson_init_static(&token, buf, len)) reject_token("'" + field + "' is not readable");
      opts.after_seq = decode_resume_token(&token);
      opts.start = ChangeStreamStart::kToken;
      continue;
    }

    if (key == "startAtOperationTime") {
      reject_second_origin(opts.start, "startAtOperationTime");
      if (!BSON_ITER_HOLDS_TIMESTAMP(&it)) {
        throw type_mismatch("$changeStream option 'startAtOperationTime' must be a Timestamp");
      }
      bson_iter_timestamp(&it, &opts.ts_t, &opts.ts_i);
      opts.start = ChangeStreamStart::kOperationTime;
      continue;
    }

    if (key == "allChangesForCluster") {
      reject_if_requested(it, "allChangesForCluster");
      continue;
    }
    if (key == "showExpandedEvents") {
      reject_if_requested(it, "showExpandedEvents");
      continue;
    }

    throw not_implemented("unsupported $changeStream option '" + std::string(key) + "'" + kPlan);
  }

  return opts;
}

}  // namespace chimera
