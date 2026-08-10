#pragma once

#include <vector>

#include "chimera/bson.h"

namespace chimera {

// MongoDB compares values of different types by a fixed type order before it
// ever looks at the values themselves, which is why {a:1} sorts before {a:"1"}.
// Returns <0, 0 or >0.
int compare_values(const bson_value_t& left, const bson_value_t& right);

// Orders documents by a Mongo sort spec — {field: 1} ascending, {field: -1}
// descending, applied left to right. A missing field sorts as null. The sort is
// stable, so equal documents keep the order the storage engine returned.
void sort_documents(std::vector<Bson>& documents, const bson_t* spec);

}  // namespace chimera
