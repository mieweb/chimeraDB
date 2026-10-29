// Exercise the real Collection::create boundary with scripted existence
// results. No server is needed: every DDL or metadata helper call is recorded.
#include <doctest/doctest.h>

#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "collection.h"
#include "chimera/error.h"
#include "oplog.h"
#include "projection.h"

namespace {
struct Call {
  std::string kind;
  std::string detail;
  const chimera::SqlSession* session;
};

std::deque<bool> existence;
std::vector<Call> calls;

struct Database {
  Database() {
    existence.clear();
    calls.clear();
  }

  void exists(bool present) { existence.push_back(present); }

  size_t side_effects() const {
    size_t count = 0;
    for (const auto& call : calls)
      if (call.kind != "exists" && call.kind != "authorize") ++count;
    return count;
  }
};

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}
}  // namespace

namespace chimera {
SqlSession::SqlSession() = default;
SqlSession::~SqlSession() = default;
SqlSession::TableLock::~TableLock() = default;

ResultSet SqlSession::query(const std::string& statement) {
  REQUIRE(starts_with(statement, "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = "));
  REQUIRE_FALSE(existence.empty());
  calls.push_back({"exists", statement, this});
  const bool present = existence.front();
  existence.pop_front();
  ResultSet result;
  if (present) result.rows = {{"1"}};
  return result;
}

void SqlSession::exec(const std::string& statement) {
  calls.push_back({"exec", statement, this});
}

std::string SqlSession::quote(std::string_view text) const {
  std::string result = "'";
  for (const char c : text) {
    if (c == '\'') result += '\'';
    result += c;
  }
  return result + "'";
}

void install_projection_support(SqlSession& sql) {
  calls.push_back({"projection", "", &sql});
}

void install_oplog_triggers(SqlSession& sql, const Namespace& ns) {
  calls.push_back({"triggers", ns.text(), &sql});
}

std::string quote_identifier(std::string_view name) {
  return "`" + std::string(name) + "`";
}

// Collection.cc also contains the data methods. Satisfy their SQL symbols at
// link time, but fail explicitly if a creation test reaches any of them.
std::string SqlSession::quote_binary(const uint8_t*, size_t) const {
  throw std::logic_error("creation unexpectedly quoted binary data");
}
std::string SqlSession::render(std::string_view, const std::vector<Param>&) const {
  throw std::logic_error("creation unexpectedly rendered a data statement");
}
uint64_t SqlSession::affected_rows() const {
  throw std::logic_error("creation unexpectedly inspected affected data rows");
}
std::unique_ptr<SqlSession::TableLock> SqlSession::lock_table(
    const std::string&, const std::string&, bool) {
  throw std::logic_error("creation unexpectedly acquired a data guard");
}
}  // namespace chimera

using namespace chimera;

TEST_CASE("existing collection does not require CREATE for an idempotent creation check") {
  Database database;
  SqlSession sql;
  Collection collection(sql, {"test", "items"});
  database.exists(true);
  bool checked_create = false;

  CHECK_NOTHROW(collection.create(false, [&] {
    checked_create = true;
    throw unauthorized("this account can write but cannot create");
  }));

  CHECK_FALSE(checked_create);
  CHECK(database.side_effects() == 0);
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].kind == "exists");
  CHECK(existence.empty());
}

TEST_CASE("missing collection rejects CREATE before any metadata or DDL side effect") {
  Database database;
  SqlSession sql;
  Collection collection(sql, {"test", "items"});
  database.exists(false);
  unsigned checked_create = 0;

  try {
    collection.create(false, [&] {
      ++checked_create;
      CHECK(database.side_effects() == 0);
      calls.push_back({"authorize", "denied", &sql});
      throw unauthorized("CREATE is denied");
    });
    FAIL("a missing collection was created without CREATE authorization");
  } catch (const TranslatorError& error) {
    CHECK(error.code() == 13);
    CHECK(error.code_name() == "Unauthorized");
  }

  CHECK(checked_create == 1);
  CHECK(database.side_effects() == 0);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].kind == "exists");
  CHECK(calls[1].kind == "authorize");
  CHECK(existence.empty());
}

TEST_CASE("allowed CREATE authorization precedes collection and metadata creation") {
  Database database;
  SqlSession sql;
  Collection collection(sql, {"test", "items"});
  database.exists(false);
  unsigned checked_create = 0;

  CHECK_NOTHROW(collection.create(false, [&] {
    ++checked_create;
    CHECK(database.side_effects() == 0);
    calls.push_back({"authorize", "allowed", &sql});
  }));

  CHECK(checked_create == 1);
  REQUIRE(calls.size() > 2);
  CHECK(calls[0].kind == "exists");
  CHECK(calls[1].kind == "authorize");
  bool created_table = false;
  bool installed_projection = false;
  bool installed_triggers = false;
  for (size_t i = 2; i < calls.size(); ++i) {
    const auto& call = calls[i];
    CHECK(call.session == &sql);
    if (call.kind == "exec" && starts_with(call.detail, "CREATE TABLE IF NOT EXISTS `test`.`items` "))
      created_table = true;
    if (call.kind == "projection") installed_projection = true;
    if (call.kind == "triggers") {
      CHECK(call.detail == "test.items");
      installed_triggers = true;
    }
  }
  CHECK(created_table);
  CHECK(installed_projection);
  CHECK(installed_triggers);
  CHECK(existence.empty());
}

TEST_CASE("collection disappearing after an earlier existence check still requires CREATE") {
  Database database;
  SqlSession sql;
  Collection collection(sql, {"test", "items"});
  database.exists(true);   // The command's earlier existence probe sees a table.
  database.exists(false);  // Concurrent DROP happens before actual creation.
  REQUIRE(collection.exists());
  unsigned checked_create = 0;

  CHECK_THROWS_AS(collection.create(false, [&] {
    ++checked_create;
    throw unauthorized("CREATE is denied after concurrent DROP");
  }), TranslatorError);

  CHECK(checked_create == 1);
  CHECK(database.side_effects() == 0);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].kind == "exists");
  CHECK(calls[1].kind == "exists");
  CHECK(existence.empty());
}

TEST_CASE("explicit create reports an existing namespace without CREATE side effects") {
  Database database;
  SqlSession sql;
  Collection collection(sql, {"test", "items"});
  database.exists(true);
  bool checked_create = false;

  try {
    collection.create(true, [&] { checked_create = true; });
    FAIL("explicit create accepted an existing collection");
  } catch (const TranslatorError& error) {
    CHECK(error.code() == 48);
    CHECK(error.code_name() == "NamespaceExists");
  }
  CHECK_FALSE(checked_create);
  CHECK(database.side_effects() == 0);
  CHECK(existence.empty());
}
