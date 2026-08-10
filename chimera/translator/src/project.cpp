#include "chimera/project.h"

#include <string>
#include <vector>

#include "chimera/error.h"

namespace chimera {
namespace {

bool is_truthy(const bson_value_t& v) {
  switch (v.value_type) {
    case BSON_TYPE_BOOL: return v.value.v_bool;
    case BSON_TYPE_INT32:
    case BSON_TYPE_INT64:
    case BSON_TYPE_DOUBLE: return value_as_double(v) != 0;
    default:
      throw not_implemented("only inclusion/exclusion projections are supported");
  }
}

}  // namespace

Bson project(const bson_t* document, const bson_t* spec) {
  if (spec == nullptr || bson_count_keys(spec) == 0) return Bson::copy_of(document);

  std::vector<Path> included;
  std::vector<Path> excluded;
  bool id_excluded = false;

  for (const auto& [name, value] : document_fields(spec)) {
    const bool keep = is_truthy(value.get());
    if (name == "_id") {
      id_excluded = !keep;
      continue;
    }
    (keep ? included : excluded).push_back(split_path(name));
  }

  if (!included.empty() && !excluded.empty()) {
    throw failed_to_parse("a projection cannot mix inclusion and exclusion");
  }

  Bson out;
  if (included.empty()) {
    // Exclusion: start from the whole document and remove.
    out = Bson::copy_of(document);
    for (const auto& path : excluded) out = path_unset(out.get(), path);
    if (id_excluded) out = path_unset(out.get(), {"_id"});
    return out;
  }

  // Inclusion: _id comes along unless it was explicitly switched off, and it
  // leads the document so the output field order matches what drivers expect.
  if (!id_excluded) {
    if (auto id = path_get(document, {"_id"})) {
      bson_append_value(out.get(), "_id", 3, &id->get());
    }
  }
  for (const auto& path : included) {
    if (auto value = path_get(document, path)) {
      out = path_set(out.get(), path, value->get());
    }
  }
  return out;
}

}  // namespace chimera
