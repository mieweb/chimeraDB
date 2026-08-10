#pragma once

#include <string>
#include <vector>

#include "chimera/bson.h"

namespace chimera {

struct UpdateResult {
  Bson doc;                              // the rewritten document
  std::vector<std::string> changed;      // dotted paths this update touched
};

// D6 — read-modify-write. The caller has the row locked; this applies the
// update in memory and hands back the whole new document. `update` is either an
// operator document ({$set: …}) or a full replacement document.
UpdateResult apply_update(const bson_t* doc, const bson_t* update);

}  // namespace chimera
