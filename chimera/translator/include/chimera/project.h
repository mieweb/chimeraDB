#pragma once

#include "chimera/bson.h"

namespace chimera {

// Applies a Mongo projection spec: {a: 1, b: 1} keeps those fields, {a: 0}
// drops them. The two forms cannot be mixed — `_id` is the documented exception,
// since it is included by default and so must be excludable alongside inclusions.
// An empty or null spec returns the document unchanged.
Bson project(const bson_t* document, const bson_t* spec);

}  // namespace chimera
