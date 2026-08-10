#include "collection.h"

#include <algorithm>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/filter.h"
#include "chimera/id.h"
#include "chimera/update.h"
#include "oplog.h"
#include "projection.h"

namespace chimera {
namespace {

// MariaDB identifiers cap at 64 characters, which is stricter than Mongo's
// limit — a name we cannot represent must fail loudly rather than be truncated
// into a collision.
void validate_identifier(const std::string& name, const char* what) {
  if (name.empty()) throw bad_value(std::string(what) + " may not be empty");
  if (name.size() > 64) throw bad_value(std::string(what) + " is longer than 64 characters");
  for (char c : name) {
    if (c == '`' || c == '\0' || c == '\n' || c == '/' || c == '\\') {
      throw bad_value(std::string(what) + " contains a character MariaDB cannot name: " + name);
    }
  }
}

std::string generated_column(const std::string& index_name, size_t position) {
  return "chimera_idx_" + index_name + "_" + std::to_string(position);
}

}  // namespace

std::string Namespace::table() const {
  return quote_identifier(db) + "." + quote_identifier(collection);
}

std::string Namespace::text() const { return db + "." + collection; }

Namespace parse_namespace(std::string db, std::string collection) {
  validate_identifier(db, "database name");
  validate_identifier(collection, "collection name");
  return Namespace{std::move(db), std::move(collection)};
}

Collection::Collection(SqlSession& sql, Namespace ns) : sql_(sql), ns_(std::move(ns)) {}

void Collection::bootstrap(SqlSession& sql) {
  sql.exec("CREATE DATABASE IF NOT EXISTS chimera_meta");
  sql.exec(
      "CREATE TABLE IF NOT EXISTS chimera_meta.collections ("
      " db_name VARCHAR(64) NOT NULL,"
      " coll_name VARCHAR(64) NOT NULL,"
      " projection_mode ENUM('manual','eager','lazy') NOT NULL DEFAULT 'manual',"
      " created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      " PRIMARY KEY (db_name, coll_name)) ENGINE=InnoDB");
  sql.exec(
      "CREATE TABLE IF NOT EXISTS chimera_meta.indexes ("
      " db_name VARCHAR(64) NOT NULL,"
      " coll_name VARCHAR(64) NOT NULL,"
      " index_name VARCHAR(128) NOT NULL,"
      " key_spec JSON NOT NULL,"
      " is_unique TINYINT(1) NOT NULL DEFAULT 0,"
      " PRIMARY KEY (db_name, coll_name, index_name)) ENGINE=InnoDB");
  // A DBA who has found chimera_meta has found the projection procedure too.
  install_projection_support(sql);
}

bool Collection::exists() const {
  const std::string sql =
      "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = " + sql_.quote(ns_.db) +
      " AND TABLE_NAME = " + sql_.quote(ns_.collection);
  return !sql_.query(sql).rows.empty();
}

void Collection::require_exists() const {
  if (!exists()) throw namespace_not_found("ns not found: " + ns_.text());
}

void Collection::create(bool error_if_exists) {
  if (exists()) {
    if (error_if_exists) throw namespace_exists("collection already exists: " + ns_.text());
    return;
  }
  bootstrap(sql_);
  sql_.exec("CREATE DATABASE IF NOT EXISTS " + quote_identifier(ns_.db));
  // VARBINARY over VARCHAR because an _id key is bytes, not text: ObjectIds are
  // raw and a collation must never fold two distinct ids together.
  sql_.exec("CREATE TABLE IF NOT EXISTS " + ns_.table() +
            " (_id VARBINARY(255) NOT NULL PRIMARY KEY, doc JSON NOT NULL) ENGINE=InnoDB");
  // Registering the collection and installing its oplog triggers are the same
  // step, so a table can never exist without the mirror that watches it (D7).
  install_oplog_triggers(sql_, ns_);
}

void Collection::drop() {
  // Dropping something that is not there is a no-op, not an error — the oracle
  // behaves that way and clients lean on it for idempotent teardown.
  if (!exists()) return;
  sql_.exec("DROP TABLE " + ns_.table());
  sql_.exec("DELETE FROM chimera_meta.collections WHERE db_name = " + sql_.quote(ns_.db) +
            " AND coll_name = " + sql_.quote(ns_.collection));
  sql_.exec("DELETE FROM chimera_meta.indexes WHERE db_name = " + sql_.quote(ns_.db) +
            " AND coll_name = " + sql_.quote(ns_.collection));
}

std::string Collection::where(const bson_t* filter) const {
  SqlFilter compiled = compile_filter(filter);
  return sql_.render(compiled.sql, compiled.params);
}

Value Collection::insert(const bson_t* document) {
  // Mongo assigns an _id when the client does not, and it leads the document.
  Bson stored;
  auto existing = path_get(document, {"_id"});
  if (!existing) {
    bson_oid_t oid;
    bson_oid_init(&oid, nullptr);
    BSON_APPEND_OID(stored.get(), "_id", &oid);
    bson_concat(stored.get(), document);
    existing = path_get(stored.get(), {"_id"});
  } else {
    stored = Bson::copy_of(document);
  }

  const std::string key = encode_id(existing->get());
  sql_.exec("INSERT INTO " + ns_.table() + " (_id, doc) VALUES (" +
            sql_.quote_binary(reinterpret_cast<const uint8_t*>(key.data()), key.size()) + ", " +
            sql_.quote(to_extjson(stored.get())) + ")");
  return *existing;
}

std::vector<Bson> Collection::load(const bson_t* filter) const {
  require_exists();
  ResultSet rows = sql_.query("SELECT doc FROM " + ns_.table() + " WHERE " + where(filter));
  std::vector<Bson> documents;
  documents.reserve(rows.rows.size());
  for (const auto& row : rows.rows) {
    if (row.empty() || !row[0]) continue;
    documents.push_back(from_extjson(*row[0]));
  }
  return documents;
}

uint64_t Collection::count(const bson_t* filter) const {
  if (!exists()) return 0;  // counting a missing collection is 0, not an error
  ResultSet rows = sql_.query("SELECT COUNT(*) FROM " + ns_.table() + " WHERE " + where(filter));
  if (rows.rows.empty() || !rows.rows[0][0]) return 0;
  return std::stoull(*rows.rows[0][0]);
}

void Collection::replace_document(const std::string& encoded_id, const bson_t* document) {
  sql_.exec("UPDATE " + ns_.table() + " SET doc = " + sql_.quote(to_extjson(document)) +
            " WHERE _id = " +
            sql_.quote_binary(reinterpret_cast<const uint8_t*>(encoded_id.data()),
                              encoded_id.size()));
}

UpdateOutcome Collection::update(const bson_t* filter, const bson_t* update, bool multi,
                                 bool upsert) {
  require_exists();
  UpdateOutcome outcome;

  // Read-modify-write: the update operators are BSON semantics, so the document
  // has to come back into the plugin to be rewritten (D6).
  std::vector<Bson> matches = load(filter);
  if (!multi && matches.size() > 1) matches.resize(1);

  for (const auto& document : matches) {
    outcome.matched++;
    auto id = path_get(document.get(), {"_id"});
    if (!id) throw internal_error("stored document has no _id: " + ns_.text());
    UpdateResult result = apply_update(document.get(), update);
    if (to_extjson(result.doc.get()) == to_extjson(document.get())) continue;
    replace_document(encode_id(id->get()), result.doc.get());
    outcome.modified++;
  }

  if (outcome.matched == 0 && upsert) {
    // The seed is the filter's equality fields; the update then runs against it,
    // which is how Mongo derives the inserted document.
    Bson seed;
    for (const auto& [name, value] : document_fields(filter)) {
      if (!name.empty() && name[0] == '$') continue;
      if (value.type() == BSON_TYPE_DOCUMENT) continue;  // an operator document, not a value
      bson_append_value(seed.get(), name.c_str(), static_cast<int>(name.size()), &value.get());
    }
    UpdateResult result = apply_update(seed.get(), update);
    outcome.upserted_id = insert(result.doc.get());
    outcome.upserted = true;
  }
  return outcome;
}

uint64_t Collection::remove(const bson_t* filter, bool just_one) {
  require_exists();
  if (!just_one) {
    sql_.exec("DELETE FROM " + ns_.table() + " WHERE " + where(filter));
    return sql_.affected_rows();
  }

  ResultSet rows =
      sql_.query("SELECT _id FROM " + ns_.table() + " WHERE " + where(filter) + " LIMIT 1");
  if (rows.rows.empty() || !rows.rows[0][0]) return 0;
  const std::string& key = *rows.rows[0][0];
  sql_.exec("DELETE FROM " + ns_.table() + " WHERE _id = " +
            sql_.quote_binary(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
  return sql_.affected_rows();
}

void Collection::create_index(const IndexSpec& spec) {
  require_exists();
  bootstrap(sql_);
  validate_identifier(spec.name, "index name");
  if (spec.keys.empty()) throw failed_to_parse("an index needs at least one key");

  // Already there? Mongo's createIndexes is idempotent for an identical spec.
  ResultSet existing =
      sql_.query("SELECT 1 FROM chimera_meta.indexes WHERE db_name = " + sql_.quote(ns_.db) +
                 " AND coll_name = " + sql_.quote(ns_.collection) +
                 " AND index_name = " + sql_.quote(spec.name));
  if (!existing.rows.empty()) return;

  // A generated column is what makes a document path indexable: it computes the
  // same expression the WHERE clause does, so the optimizer can use the index.
  std::string alter = "ALTER TABLE " + ns_.table();
  std::string columns;
  Bson key_spec;
  for (size_t i = 0; i < spec.keys.size(); ++i) {
    const std::string column = generated_column(spec.name, i);
    alter += (i == 0 ? " ADD COLUMN " : ", ADD COLUMN ") + quote_identifier(column) +
             " VARCHAR(255) AS (" + scalar_expr(split_path(spec.keys[i].first)) + ") VIRTUAL";
    if (i > 0) columns += ", ";
    columns += quote_identifier(column);
    BSON_APPEND_INT32(key_spec.get(), spec.keys[i].first.c_str(), spec.keys[i].second);
  }
  alter += ", ADD " + std::string(spec.unique ? "UNIQUE " : "") + "INDEX " +
           quote_identifier(spec.name) + " (" + columns + ")";
  sql_.exec(alter);

  sql_.exec("REPLACE INTO chimera_meta.indexes (db_name, coll_name, index_name, key_spec,"
            " is_unique) VALUES (" +
            sql_.quote(ns_.db) + ", " + sql_.quote(ns_.collection) + ", " +
            sql_.quote(spec.name) + ", " + sql_.quote(to_extjson(key_spec.get())) + ", " +
            (spec.unique ? "1" : "0") + ")");
}

std::vector<IndexSpec> Collection::list_indexes() const {
  require_exists();
  // Every Mongo collection has an _id index; here it is the InnoDB primary key,
  // which is why it never appears in chimera_meta.indexes. It is implicitly
  // unique and, like the oracle, does not advertise a `unique` field.
  std::vector<IndexSpec> out;
  out.push_back(IndexSpec{"_id_", {{"_id", 1}}, false});

  ResultSet rows = sql_.query(
      "SELECT index_name, key_spec, is_unique FROM chimera_meta.indexes WHERE db_name = " +
      sql_.quote(ns_.db) + " AND coll_name = " + sql_.quote(ns_.collection) +
      " ORDER BY index_name");
  for (const auto& row : rows.rows) {
    if (!row[0] || !row[1]) continue;
    IndexSpec spec;
    spec.name = *row[0];
    spec.unique = row[2] && *row[2] == "1";
    Bson keys = from_extjson(*row[1]);
    for (const auto& [name, value] : document_fields(keys.get())) {
      spec.keys.emplace_back(name, value_is_numeric(value.get()) &&
                                           value_as_double(value.get()) < 0
                                       ? -1
                                       : 1);
    }
    out.push_back(std::move(spec));
  }
  return out;
}

bool Collection::drop_index(const std::string& name) {
  require_exists();
  ResultSet rows =
      sql_.query("SELECT key_spec FROM chimera_meta.indexes WHERE db_name = " +
                 sql_.quote(ns_.db) + " AND coll_name = " + sql_.quote(ns_.collection) +
                 " AND index_name = " + sql_.quote(name));
  if (rows.rows.empty() || !rows.rows[0][0]) return false;

  const size_t key_count = bson_count_keys(from_extjson(*rows.rows[0][0]).get());
  std::string alter = "ALTER TABLE " + ns_.table() + " DROP INDEX " + quote_identifier(name);
  for (size_t i = 0; i < key_count; ++i) {
    alter += ", DROP COLUMN " + quote_identifier(generated_column(name, i));
  }
  sql_.exec(alter);
  sql_.exec("DELETE FROM chimera_meta.indexes WHERE db_name = " + sql_.quote(ns_.db) +
            " AND coll_name = " + sql_.quote(ns_.collection) +
            " AND index_name = " + sql_.quote(name));
  return true;
}

std::vector<std::string> list_databases(SqlSession& sql) {
  ResultSet rows = sql.query(
      "SELECT DISTINCT db_name FROM chimera_meta.collections ORDER BY db_name");
  std::vector<std::string> out;
  for (const auto& row : rows.rows) {
    if (row[0]) out.push_back(*row[0]);
  }
  return out;
}

std::vector<std::string> list_collections(SqlSession& sql, const std::string& db) {
  // information_schema, not the catalog: a table adopted by a DBA with raw SQL
  // is a collection too, and listCollections should say so.
  ResultSet rows = sql.query(
      "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = " + sql.quote(db) +
      " AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME");
  std::vector<std::string> out;
  for (const auto& row : rows.rows) {
    if (row[0]) out.push_back(*row[0]);
  }
  return out;
}

void drop_database(SqlSession& sql, const std::string& db) {
  validate_identifier(db, "database name");
  sql.exec("DROP DATABASE IF EXISTS " + quote_identifier(db));
  sql.exec("DELETE FROM chimera_meta.collections WHERE db_name = " + sql.quote(db));
  sql.exec("DELETE FROM chimera_meta.indexes WHERE db_name = " + sql.quote(db));
}

}  // namespace chimera
