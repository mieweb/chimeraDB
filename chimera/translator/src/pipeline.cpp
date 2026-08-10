#include "chimera/pipeline.h"

#include <algorithm>
#include <string>

#include "chimera/error.h"
#include "chimera/project.h"
#include "chimera/sort.h"

namespace chimera {
namespace {

// The stage document is always a one-field document: {$match: {...}}.
std::pair<std::string, Value> only_stage(const bson_t* stage) {
  auto fields = document_fields(stage);
  if (fields.size() != 1) {
    throw failed_to_parse("a pipeline stage specification object must contain exactly one field");
  }
  return fields.front();
}

bson_t view_of(const Value& value) {
  bson_t doc;
  bson_init_static(&doc, value.get().value.v_doc.data, value.get().value.v_doc.data_len);
  return doc;
}

// A BSON array is a document keyed "0","1",… so the pipeline reads like one.
std::vector<Bson> pipeline_stages(const bson_t* pipeline) {
  std::vector<Bson> stages;
  if (pipeline == nullptr) return stages;
  for (const auto& [index, element] : document_fields(pipeline)) {
    (void)index;
    if (element.type() != BSON_TYPE_DOCUMENT) {
      throw type_mismatch("each element of the aggregate pipeline must be a document");
    }
    bson_t view = view_of(element);
    stages.push_back(Bson::copy_of(&view));
  }
  return stages;
}

// `$sum: 1` counts, `$sum: "$field"` adds. A field path that is missing or
// non-numeric contributes nothing, which is what MongoDB does.
double sum_operand(const bson_t* document, const Value& operand) {
  if (operand.type() == BSON_TYPE_UTF8) {
    const std::string text(operand.get().value.v_utf8.str, operand.get().value.v_utf8.len);
    if (text.empty() || text[0] != '$') return 0;
    auto value = path_get(document, split_path(text.substr(1)));
    if (!value || !value_is_numeric(value->get())) return 0;
    return value_as_double(value->get());
  }
  if (value_is_numeric(operand.get())) return value_as_double(operand.get());
  throw not_implemented("$sum accepts a number or a field path");
}

// A `$group` `_id` is either a constant or a single field path. Anything richer
// (an expression object, a compound key) is refused rather than approximated.
Value group_key(const bson_t* document, const Value& spec) {
  if (spec.type() != BSON_TYPE_UTF8) {
    if (spec.type() == BSON_TYPE_DOCUMENT) {
      throw not_implemented("$group _id must be null, a constant, or a single field path");
    }
    return spec;
  }
  const std::string text(spec.get().value.v_utf8.str, spec.get().value.v_utf8.len);
  if (text.empty() || text[0] != '$') return spec;
  auto value = path_get(document, split_path(text.substr(1)));
  return value ? *value : Value();
}

std::vector<Bson> apply_group(const std::vector<Bson>& documents, const bson_t* spec) {
  auto fields = document_fields(spec);
  if (fields.empty() || fields.front().first != "_id") {
    throw failed_to_parse("a $group specification must include an _id");
  }
  const Value id_spec = fields.front().second;

  // Accumulators, in declaration order, so the output documents come out with
  // their fields in the order the pipeline asked for them.
  std::vector<std::pair<std::string, Value>> accumulators;
  for (size_t i = 1; i < fields.size(); ++i) {
    bson_t body = view_of(fields[i].second);
    auto inner = document_fields(&body);
    if (fields[i].second.type() != BSON_TYPE_DOCUMENT || inner.size() != 1 ||
        inner.front().first != "$sum") {
      throw not_implemented("$group supports the $sum accumulator only");
    }
    accumulators.emplace_back(fields[i].first, inner.front().second);
  }

  // Groups keep first-appearance order. MongoDB promises none, and callers that
  // care always add a $sort — but a stable answer is far easier to test against.
  std::vector<Value> keys;
  std::vector<std::vector<double>> totals;
  for (const auto& document : documents) {
    const Value key = group_key(document.get(), id_spec);
    size_t slot = 0;
    while (slot < keys.size() && !value_equal(keys[slot].get(), key.get())) ++slot;
    if (slot == keys.size()) {
      keys.push_back(key);
      totals.emplace_back(accumulators.size(), 0.0);
    }
    for (size_t i = 0; i < accumulators.size(); ++i) {
      totals[slot][i] += sum_operand(document.get(), accumulators[i].second);
    }
  }

  std::vector<Bson> results;
  results.reserve(keys.size());
  for (size_t slot = 0; slot < keys.size(); ++slot) {
    Bson out;
    bson_append_value(out.get(), "_id", 3, &keys[slot].get());
    for (size_t i = 0; i < accumulators.size(); ++i) {
      const Value total = Value::from_double(totals[slot][i]);
      bson_append_value(out.get(), accumulators[i].first.c_str(),
                        static_cast<int>(accumulators[i].first.size()), &total.get());
    }
    results.push_back(std::move(out));
  }
  return results;
}

int64_t positive_count(const Value& value, const char* stage) {
  if (!value_is_numeric(value.get())) {
    throw type_mismatch(std::string(stage) + " takes a numeric argument");
  }
  const double n = value_as_double(value.get());
  if (n < 0) throw bad_value(std::string(stage) + " must not be negative");
  return static_cast<int64_t>(n);
}

}  // namespace

Pipeline split_pipeline(const bson_t* pipeline) {
  Pipeline result;
  std::vector<Bson> stages = pipeline_stages(pipeline);

  std::vector<Bson> matches;
  size_t index = 0;
  for (; index < stages.size(); ++index) {
    const auto [name, body] = only_stage(stages[index].get());
    if (name != "$match") break;
    if (body.type() != BSON_TYPE_DOCUMENT) throw type_mismatch("$match takes a query document");
    bson_t view = view_of(body);
    matches.push_back(Bson::copy_of(&view));
  }

  if (matches.size() == 1) {
    result.prefilter = std::move(matches.front());
  } else if (matches.size() > 1) {
    // Consecutive $match stages are one conjunction. Merging them here means the
    // whole prefix reaches SQL as a single WHERE clause.
    bson_t array;
    BSON_APPEND_ARRAY_BEGIN(result.prefilter.get(), "$and", &array);
    for (size_t i = 0; i < matches.size(); ++i) {
      bson_append_document(&array, std::to_string(i).c_str(), -1, matches[i].get());
    }
    bson_append_array_end(result.prefilter.get(), &array);
  }

  for (; index < stages.size(); ++index) result.stages.push_back(std::move(stages[index]));
  return result;
}

std::vector<Bson> run_stages(std::vector<Bson> documents, const std::vector<Bson>& stages) {
  for (const auto& stage : stages) {
    const auto [name, body] = only_stage(stage.get());

    if (name == "$match") {
      // Reachable only after a reshaping stage, where there is no longer a `doc`
      // column to compile against. Refusing beats guessing.
      throw not_implemented("$match is supported only at the head of a pipeline");
    }
    if (name == "$project") {
      bson_t spec = view_of(body);
      for (auto& document : documents) document = project(document.get(), &spec);
    } else if (name == "$sort") {
      bson_t spec = view_of(body);
      sort_documents(documents, &spec);
    } else if (name == "$skip") {
      const size_t n = std::min<size_t>(documents.size(), positive_count(body, "$skip"));
      documents.erase(documents.begin(), documents.begin() + n);
    } else if (name == "$limit") {
      documents.resize(std::min<size_t>(documents.size(), positive_count(body, "$limit")));
    } else if (name == "$group") {
      bson_t spec = view_of(body);
      documents = apply_group(documents, &spec);
    } else if (name == "$count") {
      if (body.type() != BSON_TYPE_UTF8) throw type_mismatch("$count takes a field name");
      const std::string field(body.get().value.v_utf8.str, body.get().value.v_utf8.len);
      Bson out;
      BSON_APPEND_INT64(out.get(), field.c_str(), static_cast<int64_t>(documents.size()));
      documents.clear();
      documents.push_back(std::move(out));
    } else {
      throw not_implemented("unsupported aggregation stage: " + name);
    }
  }
  return documents;
}

}  // namespace chimera
