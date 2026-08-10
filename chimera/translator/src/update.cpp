#include "chimera/update.h"

#include <algorithm>

#include "chimera/error.h"

namespace chimera {
namespace {

std::vector<Value> current_array(const bson_t* doc, const Path& path, const char* op) {
  auto existing = path_get(doc, path);
  if (!existing) return {};
  if (existing->type() != BSON_TYPE_ARRAY) {
    throw type_mismatch(std::string(op) + " requires an array at '" + to_json_path(path) + "'");
  }
  return array_elements(existing->get());
}

// $push/$addToSet accept either a bare value or {$each: [...]}.
std::vector<Value> each_values(const bson_value_t& spec) {
  if (spec.value_type == BSON_TYPE_DOCUMENT) {
    bson_t view;
    if (bson_init_static(&view, spec.value.v_doc.data, spec.value.v_doc.data_len)) {
      bson_iter_t it;
      if (bson_iter_init_find(&it, &view, "$each")) {
        return array_elements(*bson_iter_value(&it));
      }
      for (const auto& field : document_fields(&view)) {
        if (!field.first.empty() && field.first[0] == '$') {
          throw not_implemented("modifier '" + field.first + "' is not supported");
        }
      }
    }
  }
  std::vector<Value> single;
  single.emplace_back(spec);
  return single;
}

Bson set_array(const bson_t* doc, const Path& path, const std::vector<Value>& elements) {
  Bson array = make_array(elements);
  return path_set(doc, path, Value::from_array(array.get()).get());
}

class Updater {
public:
  explicit Updater(const bson_t* doc) : doc_(Bson::copy_of(doc)) {}

  void apply_operator(const std::string& op, const bson_t* spec) {
    for (const auto& field : document_fields(spec)) {
      Path path = split_path(field.first);
      if (std::find(path.begin(), path.end(), "$") != path.end()) {
        throw not_implemented("the positional '$' operator is not supported yet");
      }
      apply_one(op, path, field.second.get());
      changed_.push_back(field.first);
    }
  }

  UpdateResult finish() { return {std::move(doc_), std::move(changed_)}; }

private:
  Bson doc_;
  std::vector<std::string> changed_;

  void apply_one(const std::string& op, const Path& path, const bson_value_t& arg) {
    if (op == "$set") {
      doc_ = path_set(doc_.get(), path, arg);
    } else if (op == "$unset") {
      doc_ = path_unset(doc_.get(), path);
    } else if (op == "$inc") {
      if (!value_is_numeric(arg)) throw type_mismatch("$inc requires a numeric argument");
      auto existing = path_get(doc_.get(), path);
      double base = 0;
      if (existing) {
        if (!value_is_numeric(existing->get())) {
          throw type_mismatch("$inc target '" + to_json_path(path) + "' is not a number");
        }
        base = value_as_double(existing->get());
      }
      double sum = base + value_as_double(arg);
      bool integral = (!existing || existing->type() != BSON_TYPE_DOUBLE) &&
                      arg.value_type != BSON_TYPE_DOUBLE;
      Value next = integral ? Value::from_int64(static_cast<int64_t>(sum))
                            : Value::from_double(sum);
      doc_ = path_set(doc_.get(), path, next.get());
    } else if (op == "$push") {
      auto elements = current_array(doc_.get(), path, "$push");
      for (auto& v : each_values(arg)) elements.push_back(std::move(v));
      doc_ = set_array(doc_.get(), path, elements);
    } else if (op == "$addToSet") {
      auto elements = current_array(doc_.get(), path, "$addToSet");
      for (auto& candidate : each_values(arg)) {
        bool present = std::any_of(elements.begin(), elements.end(), [&](const Value& e) {
          return value_equal(e.get(), candidate.get());
        });
        if (!present) elements.push_back(std::move(candidate));
      }
      doc_ = set_array(doc_.get(), path, elements);
    } else if (op == "$pull") {
      if (arg.value_type == BSON_TYPE_DOCUMENT) {
        throw not_implemented("$pull with a condition document is not supported yet");
      }
      auto elements = current_array(doc_.get(), path, "$pull");
      elements.erase(std::remove_if(elements.begin(), elements.end(),
                                    [&](const Value& e) { return value_equal(e.get(), arg); }),
                     elements.end());
      doc_ = set_array(doc_.get(), path, elements);
    } else if (op == "$pop") {
      auto elements = current_array(doc_.get(), path, "$pop");
      if (elements.empty()) return;
      if (value_is_numeric(arg) && value_as_double(arg) < 0) {
        elements.erase(elements.begin());
      } else {
        elements.pop_back();
      }
      doc_ = set_array(doc_.get(), path, elements);
    } else {
      throw not_implemented("unsupported update operator '" + op + "'");
    }
  }
};

}  // namespace

UpdateResult apply_update(const bson_t* doc, const bson_t* update) {
  auto top = document_fields(update);
  bool has_operators = false;
  for (const auto& field : top) {
    bool is_operator = !field.first.empty() && field.first[0] == '$';
    if (is_operator != has_operators && &field != &top.front()) {
      throw failed_to_parse("update document mixes operators with replacement fields");
    }
    has_operators = is_operator;
  }

  if (!has_operators) {
    // Replacement: the new document wins, but _id is immutable.
    Bson replacement = Bson::copy_of(update);
    auto id = path_get(doc, {"_id"});
    if (id) replacement = path_set(replacement.get(), {"_id"}, id->get());
    std::vector<std::string> changed;
    for (const auto& field : top) changed.push_back(field.first);
    return {std::move(replacement), std::move(changed)};
  }

  Updater updater(doc);
  for (const auto& field : top) {
    if (field.second.type() != BSON_TYPE_DOCUMENT) {
      throw failed_to_parse("update operator '" + field.first + "' expects a document");
    }
    bson_t view;
    if (!bson_init_static(&view, field.second.get().value.v_doc.data,
                          field.second.get().value.v_doc.data_len)) {
      throw failed_to_parse("malformed update operator document");
    }
    updater.apply_operator(field.first, &view);
  }
  return updater.finish();
}

}  // namespace chimera
