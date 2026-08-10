#include "chimera/codec.h"

#include "chimera/error.h"

namespace chimera {

std::string to_extjson(const bson_t* doc) {
  size_t length = 0;
  char* json = bson_as_canonical_extended_json(doc, &length);
  if (!json) throw bad_value("document cannot be serialized as extended JSON");
  std::string out(json, length);
  bson_free(json);
  return out;
}

Bson from_extjson(std::string_view extjson) {
  bson_error_t error;
  bson_t* doc = bson_new_from_json(reinterpret_cast<const uint8_t*>(extjson.data()),
                                   static_cast<ssize_t>(extjson.size()), &error);
  if (!doc) throw failed_to_parse(error.message);
  return Bson(doc);
}

}  // namespace chimera
