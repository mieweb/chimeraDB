#pragma once

#include <string>

#include "chimera/bson.h"

namespace chimera {

// M2.3 — the `_id` VARBINARY(255) primary key.
//
// A collection's primary key must be a single byte string, but Mongo ids are
// not all one type. Every id is encoded as one type-tag byte (the BSON type
// byte, so the encoding reads itself) followed by a payload chosen so that
// memcmp ordering is stable and meaningful within a type:
//
//   0x07 + 12 raw bytes             ObjectId
//   0x02 + UTF-8 bytes              string  (Meteor's default random ids)
//   0x12 + 8 bytes, big-endian,     any integer — int32 and int64 share the tag
//          sign bit flipped         so `1` and `NumberLong(1)` are one key
//
// Integral ids therefore decode back as int64. That normalization is the point:
// it is what makes duplicate-key detection agree with MongoDB's numeric
// comparison. Every other BSON type is rejected rather than silently mangled.
std::string encode_id(const bson_value_t& id);
Value decode_id(const std::string& key);

}  // namespace chimera
