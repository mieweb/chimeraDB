#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chimera/bson.h"
#include "sql.h"

namespace chimera {

// A Mongo database is a MariaDB database and a Mongo collection is a table, so
// a namespace is just a qualified table name (D2).
struct Namespace {
  std::string db;
  std::string collection;

  std::string table() const;  // `db`.`coll`, ready to paste into SQL
  std::string text() const;   // db.coll, as the wire protocol spells it
};

// Rejects anything that could not be a MariaDB identifier, so a namespace is
// safe to interpolate once it exists.
Namespace parse_namespace(std::string db, std::string collection);

struct UpdateOutcome {
  uint64_t matched = 0;
  uint64_t modified = 0;
  bool upserted = false;
  Value upserted_id;
};

struct IndexSpec {
  std::string name;
  std::vector<std::pair<std::string, int>> keys;  // path, 1 | -1
  bool unique = false;
};

// One collection table, and every operation that touches it. Reads pull the
// filter down into SQL; ordering, projection and paging happen above this layer
// because they need BSON semantics MariaDB's JSON functions cannot express.
class Collection {
public:
  Collection(SqlSession& sql, Namespace ns);

  // Creates chimera_meta if it is missing. Cheap enough to call per command.
  static void bootstrap(SqlSession& sql);

  bool exists() const;
  void create(bool error_if_exists);
  void drop();

  Value insert(const bson_t* document);
  std::vector<Bson> load(const bson_t* filter) const;
  uint64_t count(const bson_t* filter) const;
  UpdateOutcome update(const bson_t* filter, const bson_t* update, bool multi, bool upsert);
  uint64_t remove(const bson_t* filter, bool just_one);

  void create_index(const IndexSpec& spec);
  std::vector<IndexSpec> list_indexes() const;
  bool drop_index(const std::string& name);

  const Namespace& ns() const { return ns_; }

private:
  std::string where(const bson_t* filter) const;
  void require_exists() const;
  void replace_document(const std::string& encoded_id, const bson_t* document);

  SqlSession& sql_;
  Namespace ns_;
};

std::vector<std::string> list_databases(SqlSession& sql);
std::vector<std::string> list_collections(SqlSession& sql, const std::string& db);
void drop_database(SqlSession& sql, const std::string& db);

}  // namespace chimera
