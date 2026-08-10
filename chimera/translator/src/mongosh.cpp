#include "chimera/mongosh.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "chimera/error.h"

namespace chimera {
namespace {

// A hand-written recursive-descent reader. mongosh's argument syntax is a small
// superset of JSON, and the alternative — feeding it to a JSON parser after a
// pile of regex repairs — fails on exactly the inputs that motivate the feature.
class Reader {
public:
  explicit Reader(std::string_view text) : text_(text) {}

  void skip_space() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool eof() {
    skip_space();
    return pos_ >= text_.size();
  }

  char peek() {
    skip_space();
    if (pos_ >= text_.size()) fail("unexpected end of statement");
    return text_[pos_];
  }

  void expect(char c) {
    if (peek() != c) fail(std::string("expected '") + c + "'");
    ++pos_;
  }

  bool consume(char c) {
    if (eof() || peek() != c) return false;
    ++pos_;
    return true;
  }

  // A JavaScript identifier, which is also what an unquoted key looks like.
  std::string identifier() {
    skip_space();
    const size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) fail("expected a name");
    return std::string(text_.substr(start, pos_ - start));
  }

  std::string quoted_string() {
    const char quote = peek();
    if (quote != '\'' && quote != '"') fail("expected a quoted string");
    ++pos_;
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) fail("unterminated string");
      const char c = text_[pos_++];
      if (c == quote) break;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) fail("unterminated escape");
      const char escaped = text_[pos_++];
      switch (escaped) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        default: out.push_back(escaped); break;
      }
    }
    return out;
  }

  Value value() {
    const char c = peek();
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '\'' || c == '"') return Value::from_utf8(quoted_string());
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return number();
    return word();
  }

  [[noreturn]] void fail(const std::string& what) {
    throw failed_to_parse(what + " at offset " + std::to_string(pos_));
  }

private:
  Value object() {
    expect('{');
    Bson doc;
    if (!consume('}')) {
      do {
        skip_space();
        const char c = peek();
        const std::string key = (c == '\'' || c == '"') ? quoted_string() : identifier();
        expect(':');
        const Value v = value();
        bson_append_value(doc.get(), key.c_str(), static_cast<int>(key.size()), &v.get());
      } while (consume(','));
      expect('}');
    }
    return Value::from_document(doc.get());
  }

  Value array() {
    expect('[');
    Bson arr;
    if (!consume(']')) {
      uint32_t index = 0;
      do {
        const Value v = value();
        const char* key = nullptr;
        char buffer[16];
        const size_t length = bson_uint32_to_string(index++, &key, buffer, sizeof buffer);
        bson_append_value(arr.get(), key, static_cast<int>(length), &v.get());
      } while (consume(','));
      expect(']');
    }
    return Value::from_array(arr.get());
  }

  Value number() {
    skip_space();
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    bool real = false;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        real = real || c == '.' || c == 'e' || c == 'E';
        ++pos_;
      } else {
        break;
      }
    }
    const std::string literal(text_.substr(start, pos_ - start));
    // An integer stays an integer: `{qty: 3}` typed in a shell must match the
    // same document that `{qty: 3}` over the wire inserted.
    if (real) return Value::from_double(std::strtod(literal.c_str(), nullptr));
    return Value::from_int64(std::strtoll(literal.c_str(), nullptr, 10));
  }

  // Bare words: the three JSON literals, plus the constructor that appears in
  // almost every real pasted statement.
  Value word() {
    const std::string name = identifier();
    if (name == "true" || name == "false") {
      bson_value_t v;
      v.value_type = BSON_TYPE_BOOL;
      v.value.v_bool = name == "true";
      return Value(v);
    }
    if (name == "null") {
      bson_value_t v;
      v.value_type = BSON_TYPE_NULL;
      return Value(v);
    }
    if (name == "ObjectId") {
      expect('(');
      const std::string hex = quoted_string();
      expect(')');
      if (!bson_oid_is_valid(hex.c_str(), hex.size())) {
        throw bad_value("'" + hex + "' is not a valid ObjectId");
      }
      bson_value_t v;
      v.value_type = BSON_TYPE_OID;
      bson_oid_init_from_string(&v.value.v_oid, hex.c_str());
      return Value(v);
    }
    // Everything else in the shell's vocabulary — ISODate, NumberDecimal, a
    // variable name — would need an evaluator, and guessing is worse than
    // saying so.
    throw not_implemented("unsupported value '" + name + "'");
  }

  std::string_view text_;
  size_t pos_ = 0;
};

}  // namespace

ShellCall parse_shell_call(std::string_view statement) {
  Reader reader(statement);
  ShellCall call;

  if (reader.identifier() != "db") reader.fail("a statement must start with 'db.'");
  reader.expect('.');
  call.collection = reader.identifier();
  reader.expect('.');
  call.verb = reader.identifier();

  reader.expect('(');
  if (!reader.consume(')')) {
    do {
      call.args.push_back(reader.value());
    } while (reader.consume(','));
    reader.expect(')');
  }

  // A trailing semicolon is how the statement looked in the shell; anything
  // else after the call is a second statement we would silently ignore.
  reader.consume(';');
  if (!reader.eof()) reader.fail("trailing input after the statement");
  return call;
}

namespace {

const bson_value_t* argument(const ShellCall& call, size_t index) {
  return index < call.args.size() ? &call.args[index].get() : nullptr;
}

// A filter or update argument must be a document; an omitted one is an empty
// document, which is how a shell reads `find()`.
void append_document_argument(bson_t* command, const char* name, const ShellCall& call,
                              size_t index) {
  const bson_value_t* value = argument(call, index);
  if (value == nullptr) {
    bson_t empty;
    bson_init(&empty);
    bson_append_document(command, name, -1, &empty);
    bson_destroy(&empty);
    return;
  }
  if (value->value_type != BSON_TYPE_DOCUMENT) {
    throw type_mismatch(std::string("argument ") + std::to_string(index + 1) + " of " +
                        call.verb + "() must be a document");
  }
  bson_append_value(command, name, -1, value);
}

// `documents`, `updates` and `deletes` are all arrays of one thing; this builds
// the single-element case they share.
void append_singleton_array(bson_t* command, const char* name, const bson_t* element) {
  bson_t array;
  bson_append_array_begin(command, name, -1, &array);
  bson_append_document(&array, "0", 1, element);
  bson_append_array_end(command, &array);
}

Bson write_statement(const ShellCall& call, const bson_value_t* update, bool multi) {
  Bson statement;
  append_document_argument(statement.get(), "q", call, 0);
  if (update == nullptr || update->value_type != BSON_TYPE_DOCUMENT) {
    throw type_mismatch(call.verb + "() needs an update document");
  }
  bson_append_value(statement.get(), "u", -1, update);
  BSON_APPEND_BOOL(statement.get(), "multi", multi);
  return statement;
}

Bson delete_statement(const ShellCall& call, int32_t limit) {
  Bson statement;
  append_document_argument(statement.get(), "q", call, 0);
  BSON_APPEND_INT32(statement.get(), "limit", limit);
  return statement;
}

void append_projection(bson_t* command, const ShellCall& call) {
  // The second argument of find()/findOne() is a projection, as it has been in
  // every shell since the beginning. Sorting and paging are `aggregate`'s job
  // here, because a chained `.sort()` is a method call and this is not an
  // evaluator.
  if (const bson_value_t* projection = argument(call, 1)) {
    if (projection->value_type != BSON_TYPE_DOCUMENT) {
      throw type_mismatch("the second argument of " + call.verb +
                          "() must be a projection document");
    }
    bson_append_value(command, "projection", -1, projection);
  }
}

void reject_extra_arguments(const ShellCall& call, size_t allowed) {
  if (call.args.size() > allowed) {
    throw not_implemented(call.verb + "() takes at most " +
                          std::to_string(allowed) + " argument(s) here");
  }
}

}  // namespace

ShellCommand build_shell_command(const ShellCall& call) {
  ShellCommand out;
  bson_t* c = out.command.get();
  const char* coll = call.collection.c_str();

  if (call.verb == "find" || call.verb == "findOne") {
    reject_extra_arguments(call, 2);
    BSON_APPEND_UTF8(c, "find", coll);
    append_document_argument(c, "filter", call, 0);
    append_projection(c, call);
    if (call.verb == "findOne") {
      BSON_APPEND_INT64(c, "limit", 1);
      out.result = ShellResult::FirstDocument;
    } else {
      out.result = ShellResult::Documents;
    }
    return out;
  }

  if (call.verb == "insertOne" || call.verb == "insertMany") {
    reject_extra_arguments(call, 1);
    BSON_APPEND_UTF8(c, "insert", coll);
    const bson_value_t* value = argument(call, 0);
    if (call.verb == "insertOne") {
      if (value == nullptr || value->value_type != BSON_TYPE_DOCUMENT) {
        throw type_mismatch("insertOne() needs a document");
      }
      bson_t document;
      bson_init_static(&document, value->value.v_doc.data, value->value.v_doc.data_len);
      append_singleton_array(c, "documents", &document);
    } else {
      if (value == nullptr || value->value_type != BSON_TYPE_ARRAY) {
        throw type_mismatch("insertMany() needs an array of documents");
      }
      bson_append_value(c, "documents", -1, value);
    }
    return out;
  }

  if (call.verb == "updateOne" || call.verb == "updateMany" || call.verb == "replaceOne") {
    reject_extra_arguments(call, 2);
    BSON_APPEND_UTF8(c, "update", coll);
    const Bson statement =
        write_statement(call, argument(call, 1), call.verb == "updateMany");
    append_singleton_array(c, "updates", statement.get());
    return out;
  }

  if (call.verb == "deleteOne" || call.verb == "deleteMany") {
    reject_extra_arguments(call, 1);
    BSON_APPEND_UTF8(c, "delete", coll);
    const Bson statement = delete_statement(call, call.verb == "deleteOne" ? 1 : 0);
    append_singleton_array(c, "deletes", statement.get());
    return out;
  }

  if (call.verb == "countDocuments" || call.verb == "count") {
    reject_extra_arguments(call, 1);
    BSON_APPEND_UTF8(c, "count", coll);
    append_document_argument(c, "query", call, 0);
    out.result = ShellResult::Count;
    return out;
  }

  if (call.verb == "aggregate") {
    reject_extra_arguments(call, 1);
    BSON_APPEND_UTF8(c, "aggregate", coll);
    const bson_value_t* pipeline = argument(call, 0);
    if (pipeline == nullptr || pipeline->value_type != BSON_TYPE_ARRAY) {
      throw type_mismatch("aggregate() needs a pipeline array");
    }
    bson_append_value(c, "pipeline", -1, pipeline);
    bson_t cursor;
    BSON_APPEND_DOCUMENT_BEGIN(c, "cursor", &cursor);
    bson_append_document_end(c, &cursor);
    out.result = ShellResult::Documents;
    return out;
  }

  throw not_implemented("unsupported verb '" + call.verb + "()'");
}

}  // namespace chimera
