#pragma once

#include <string>
#include <string_view>

#include "chimera/bson.h"

namespace chimera {

// D5: documents are stored as MongoDB canonical Extended JSON in a MariaDB
// JSON column. These are the only two functions that cross that boundary.
std::string to_extjson(const bson_t* doc);
Bson from_extjson(std::string_view extjson);

}  // namespace chimera
