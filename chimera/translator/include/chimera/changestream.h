#pragma once

#include <bson/bson.h>

#include <cstdint>
#include <optional>
#include <string>

namespace chimera {

// The decidable half of `$changeStream`: what a resume token means, and which
// stage options we can honour. Both are pure, so they live here rather than in
// the plugin — the same M7.1 reasoning that put the SQL guard in the translator.
//
// The oplog's `seq` is the whole of a chimera resume token. A real MongoDB token
// packs a keystring (clusterTime, txn ordinal, uuid, documentKey) into `_data`;
// ours does not have to, because a set of one has a single total order and `seq`
// already is it. Tokens are opaque to drivers by contract, so this is a
// difference no client can observe — except in width, so the encoding is fixed
// at 16 hex digits and rejects anything else rather than guessing.

std::string encode_resume_token(uint64_t seq);

// Throws (as a non-resumable error, which is how a driver classifies a bad
// token) when `token` is not a document holding a well-formed `_data` string.
uint64_t decode_resume_token(const bson_t* token);

// Where a stream begins. Exactly one of these may be requested; `kHead` is the
// default and means "only what happens next".
enum class ChangeStreamStart { kHead, kToken, kOperationTime };

struct ChangeStreamOptions {
  ChangeStreamStart start = ChangeStreamStart::kHead;
  uint64_t after_seq = 0;   // kToken: deliver strictly after this seq
  uint32_t ts_t = 0;        // kOperationTime: deliver at or after this timestamp
  uint32_t ts_i = 0;
};

// Parses the body of a `$changeStream` stage. Anything we cannot serve raises
// `not_implemented` naming changestream-plan.md, never a silent no-op: a stream
// that quietly ignores an option looks like it is working and loses events.
ChangeStreamOptions parse_change_stream(const bson_t* stage);

}  // namespace chimera
