// Exercise the real retention transaction with a scripted SQL adapter. These
// tests verify SQL ordering and failure boundaries; integration tests execute
// the statements against MariaDB and cover durable state across restarts.
#include <doctest/doctest.h>

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "chimera/error.h"
#include "oplog.h"

namespace {
struct Call {
  std::string kind;
  std::string statement;
  const chimera::SqlSession* session;
  bool in_transaction;
};

std::vector<Call> calls;
std::deque<std::variant<chimera::ResultSet, chimera::TranslatorError>> replies;
const chimera::SqlSession* transaction_session = nullptr;
const chimera::SqlSession* caller_transaction = nullptr;
std::vector<const chimera::SqlSession*> transaction_probes;
std::string fail_statement;
uint64_t affected = 0;

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

void record(const char* kind, const std::string& statement, const chimera::SqlSession* session) {
  calls.push_back({kind, statement, session, transaction_session == session});
}

std::vector<Call> matching(const std::string& prefix) {
  std::vector<Call> result;
  for (const auto& call : calls) {
    if (starts_with(call.statement, prefix)) result.push_back(call);
  }
  return result;
}

struct Database {
  Database() {
    calls.clear();
    replies.clear();
    transaction_session = nullptr;
    caller_transaction = nullptr;
    transaction_probes.clear();
    fail_statement.clear();
    affected = 0;
  }

  void rows(std::vector<chimera::Row> values) {
    chimera::ResultSet result;
    result.rows = std::move(values);
    replies.emplace_back(std::move(result));
  }

  void pruning_locks() {
    rows({{"1"}});  // clock row locked only after planning found work
    rows({{"0"}});  // durable pruning metadata exists
  }

  void one_row_plan() {
    rows({{"100"}});  // committed head fences every preflight read and delete
    rows({{"100"}});  // retain one actual event, despite the sequence gap
    rows({{"1"}});    // actual last deleted event, not numeric cutoff 99
    rows({{"900", "7"}});
  }

  void one_row_candidate() {
    one_row_plan();
    pruning_locks();
    rows({{"1"}});  // revalidate the planned candidate by primary key
    affected = 1;
  }
};
}  // namespace

namespace chimera {
SqlSession::SqlSession() = default;
SqlSession::~SqlSession() = default;
SqlThreadScope::SqlThreadScope() = default;
SqlThreadScope::~SqlThreadScope() = default;

ResultSet SqlSession::query(const std::string& statement) {
  record("query", statement, this);
  REQUIRE_FALSE(replies.empty());
  auto result = std::move(replies.front());
  replies.pop_front();
  if (const auto* error = std::get_if<TranslatorError>(&result)) throw *error;
  return std::move(std::get<ResultSet>(result));
}

void SqlSession::exec(const std::string& statement) {
  record("exec", statement, this);
  if (!fail_statement.empty() && starts_with(statement, fail_statement)) {
    throw internal_error("injected SQL failure");
  }
}

uint64_t SqlSession::affected_rows() const { return affected; }
bool SqlSession::in_transaction() const {
  transaction_probes.push_back(this);
  return this == caller_transaction || this == transaction_session;
}
std::string SqlSession::render(std::string_view statement, const std::vector<Param>&) const {
  return std::string(statement);
}
std::string Namespace::text() const { return db + "." + collection; }

void SqlSession::begin() {
  REQUIRE(transaction_session == nullptr);
  record("begin", "BEGIN", this);
  transaction_session = this;
}
void SqlSession::commit() {
  REQUIRE(transaction_session == this);
  record("commit", "COMMIT", this);
  transaction_session = nullptr;
}
void SqlSession::rollback() {
  REQUIRE(transaction_session == this);
  record("rollback", "ROLLBACK", this);
  transaction_session = nullptr;
}
}  // namespace chimera

using namespace chimera;

TEST_CASE("disabled pruning does not open a transaction or advance history") {
  Database database;
  SqlSession sql;
  CHECK(prune_oplog(sql, 0, 0) == 0);
  CHECK(calls.empty());
}

TEST_CASE("the default row-cap scan never locks writers when fewer events exist") {
  Database database;
  SqlSession sql;
  database.rows({{"100"}});
  database.rows({});  // fewer than 100,000 actual retained events
  CHECK(prune_oplog(sql, 100000, 0) == 0);
  REQUIRE(matching("SELECT seq FROM chimera_meta.oplog ORDER BY seq DESC LIMIT 1 OFFSET 99999").size() == 1);
  CHECK(matching("DELETE").empty());
  CHECK(matching("UPDATE chimera_meta.oplog_history").empty());
  CHECK(matching("BEGIN").empty());
  CHECK(matching("SELECT id FROM chimera_meta.oplog_clock").empty());
  for (const auto& call : calls) CHECK_FALSE(call.in_transaction);
  CHECK(replies.empty());
}

TEST_CASE("a prune with no eligible rows leaves deletion watermarks untouched") {
  Database database;
  SqlSession sql;
  database.rows({{"100"}});
  database.rows({{"100"}});
  database.rows({});  // only the newest event remains
  CHECK(prune_oplog(sql, 1, 0) == 0);
  CHECK(matching("DELETE").empty());
  CHECK(matching("UPDATE chimera_meta.oplog_history").empty());
  CHECK(matching("BEGIN").empty());
  CHECK(matching("SELECT id FROM chimera_meta.oplog_clock").empty());
  for (const auto& call : calls) CHECK_FALSE(call.in_transaction);
  CHECK(replies.empty());
}

TEST_CASE("pruning plans outside writer locks then atomically deletes verified history") {
  Database database;
  SqlSession sql;
  database.one_row_candidate();
  CHECK(prune_oplog(sql, 1, 0) == 1);
  REQUIRE(calls.size() >= 9);
  CHECK(calls[0].statement == "SELECT COALESCE(MAX(seq), 0) FROM chimera_meta.oplog");
  CHECK(calls[1].statement == "SELECT seq FROM chimera_meta.oplog ORDER BY seq DESC LIMIT 1 OFFSET 0");
  CHECK(calls[4].kind == "begin");
  CHECK(calls[5].statement == "SELECT id FROM chimera_meta.oplog_clock WHERE id = 1 FOR UPDATE");
  CHECK(calls[6].statement == "SELECT pruned_seq FROM chimera_meta.oplog_history WHERE id = 1 FOR UPDATE");
  CHECK(calls[7].statement == "SELECT seq FROM chimera_meta.oplog WHERE seq = 1 AND (seq < 100 AND (seq < 100))");
  CHECK(calls.back().kind == "commit");
  const auto deleted = matching("DELETE FROM chimera_meta.oplog");
  const auto updated = matching("UPDATE chimera_meta.oplog_history");
  REQUIRE(deleted.size() == 1);
  REQUIRE(updated.size() == 1);
  CHECK(deleted[0].in_transaction);
  CHECK(deleted[0].statement == "DELETE FROM chimera_meta.oplog WHERE seq <= 1 AND (seq < 100 AND (seq < 100))");
  CHECK(updated[0].in_transaction);
  CHECK(updated[0].statement.find("GREATEST(pruned_seq, 1)") != std::string::npos);
  CHECK(updated[0].statement.find("GREATEST(pruned_ts_t, 900)") != std::string::npos);
  CHECK(updated[0].statement.find("GREATEST(pruned_ts_i, 7)") != std::string::npos);
  // MariaDB assignment order must not overwrite seconds before deciding how
  // to update the timestamp increment.
  CHECK(updated[0].statement.find("pruned_ts_i =") <
        updated[0].statement.find("pruned_ts_t = GREATEST"));
  bool saw_delete = false;
  bool saw_begin = false;
  for (const auto& call : calls) {
    CHECK(call.session == &sql);
    CHECK(call.in_transaction == saw_begin);
    if (call.kind == "begin") saw_begin = true;
    // All range/order/offset candidate reads precede BEGIN. Only the constant
    // metadata and candidate primary-key lookups are allowed under the lock.
    if (call.kind == "query" && call.statement.find("ORDER BY") != std::string::npos) {
      CHECK_FALSE(call.in_transaction);
    }
    if (starts_with(call.statement, "DELETE")) saw_delete = true;
    if (starts_with(call.statement, "UPDATE")) CHECK(saw_delete);
  }
  CHECK(replies.empty());
}

TEST_CASE("age selection and deletion share one sampled cutoff and preserve the newest row") {
  Database database;
  SqlSession sql;
  database.rows({{"100"}});
  database.rows({{"1000"}});  // the clock can advance after this snapshot
  database.rows({{"7"}});
  database.rows({{"985", "9"}});
  database.pruning_locks();
  database.rows({{"7"}});
  affected = 2;
  CHECK(prune_oplog(sql, 0, 10) == 2);
  REQUIRE(matching("SELECT UNIX_TIMESTAMP()").size() == 1);
  const std::string predicate = "seq < 100 AND (ts_t < 990)";
  const auto candidates = matching("SELECT seq FROM chimera_meta.oplog WHERE seq <");
  const auto timestamps = matching("SELECT ts_t, ts_i FROM chimera_meta.oplog WHERE ");
  const auto deleted = matching("DELETE FROM chimera_meta.oplog WHERE ");
  REQUIRE(candidates.size() == 1);
  REQUIRE(timestamps.size() == 1);
  REQUIRE(deleted.size() == 1);
  CHECK(candidates[0].statement == "SELECT seq FROM chimera_meta.oplog WHERE " + predicate +
                                    " ORDER BY seq DESC LIMIT 1");
  CHECK(timestamps[0].statement == "SELECT ts_t, ts_i FROM chimera_meta.oplog WHERE " + predicate +
                                    " ORDER BY ts_t DESC, ts_i DESC LIMIT 1");
  CHECK_FALSE(candidates[0].in_transaction);
  CHECK_FALSE(timestamps[0].in_transaction);
  CHECK(deleted[0].statement == "DELETE FROM chimera_meta.oplog WHERE seq <= 7 AND (" + predicate + ")");
  CHECK(replies.empty());
}

TEST_CASE("an empty delete does not publish selected but undeleted history") {
  Database database;
  SqlSession sql;
  database.one_row_candidate();
  affected = 0;
  CHECK(prune_oplog(sql, 1, 0) == 0);
  CHECK(matching("DELETE").size() == 1);
  CHECK(matching("UPDATE chimera_meta.oplog_history").empty());
  CHECK(matching("COMMIT").size() == 1);
}

TEST_CASE("delete and watermark failures roll back the entire pruning transaction") {
  for (const auto& failure : {"DELETE FROM chimera_meta.oplog", "UPDATE chimera_meta.oplog_history"}) {
    CAPTURE(failure);
    Database database;
    SqlSession sql;
    database.one_row_candidate();
    fail_statement = failure;
    CHECK_THROWS_AS(prune_oplog(sql, 1, 0), TranslatorError);
    REQUIRE_FALSE(calls.empty());
    CHECK(calls.back().kind == "rollback");
    CHECK(matching("ROLLBACK").size() == 1);
    CHECK(matching("COMMIT").empty());
    CHECK(transaction_session == nullptr);
    if (starts_with(failure, "DELETE")) {
      CHECK(matching("UPDATE chimera_meta.oplog_history").empty());
    }
  }
}

TEST_CASE("missing clock or history metadata refuses pruning before any deletion") {
  for (bool missing_clock : {false, true}) {
    CAPTURE(missing_clock);
    Database database;
    SqlSession sql;
    database.one_row_plan();
    if (!missing_clock) database.rows({{"1"}});
    database.rows({});
    CHECK_THROWS_AS(prune_oplog(sql, 1, 0), TranslatorError);
    CHECK(matching("DELETE").empty());
    CHECK(matching("COMMIT").empty());
    CHECK(matching("ROLLBACK").size() == 1);
    CHECK(replies.empty());
  }
}

TEST_CASE("an empty oplog or unexpired age policy never locks writers") {
  for (bool empty : {false, true}) {
    Database database;
    SqlSession sql;
    database.rows({{empty ? "0" : "100"}});
    if (!empty) database.rows({{"10"}});  // max age exceeds current time
    CHECK(prune_oplog(sql, 0, 20) == 0);
    CHECK(matching("BEGIN").empty());
    CHECK(matching("SELECT id FROM chimera_meta.oplog_clock").empty());
    CHECK(replies.empty());
  }
}

TEST_CASE("a candidate lost before locked verification never deletes or publishes history") {
  Database database;
  SqlSession sql;
  database.one_row_plan();
  database.pruning_locks();
  database.rows({});
  CHECK(prune_oplog(sql, 1, 0) == 0);
  CHECK(matching("DELETE").empty());
  CHECK(matching("UPDATE chimera_meta.oplog_history").empty());
  CHECK(matching("COMMIT").size() == 1);
  CHECK(replies.empty());
}

TEST_CASE("commits during planning cannot extend deletion beyond the sampled head") {
  Database database;
  SqlSession sql;
  database.rows({{"100"}});       // sampled before the new commits
  database.rows({{"200"}});       // newest kept row after concurrent appends
  database.rows({{"99"}});        // selected below the frozen head
  database.rows({{"900", "7"}});
  database.pruning_locks();
  database.rows({{"99"}});
  affected = 2;
  CHECK(prune_oplog(sql, 1, 0) == 2);
  REQUIRE(matching("SELECT COALESCE(MAX(seq)").size() == 1);
  const auto deleted = matching("DELETE FROM chimera_meta.oplog");
  REQUIRE(deleted.size() == 1);
  CHECK(deleted[0].statement ==
        "DELETE FROM chimera_meta.oplog WHERE seq <= 99 AND (seq < 100 AND (seq < 200))");
  CHECK(replies.empty());
}

TEST_CASE("initialized oplog schema checks its own session without DDL or writer locks") {
  Database database;
  SqlSession caller;
  database.rows({{"1"}});
  install_oplog_schema(caller);
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].kind == "query");
  CHECK(calls[0].session != &caller);
  CHECK(calls[0].statement == "SELECT id FROM chimera_meta.oplog_history WHERE id = 1");
}

TEST_CASE("initialized history remains usable inside the caller transaction") {
  Database database;
  SqlSession caller;
  caller_transaction = &caller;
  database.rows({{"1"}});
  CHECK_NOTHROW(install_oplog_schema(caller));
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].kind == "query");
  CHECK(calls[0].session != &caller);
  CHECK(transaction_probes.empty());
  CHECK(caller_transaction == &caller);
}

TEST_CASE("missing history in a caller transaction refuses migration before DDL or locks") {
  for (bool missing_table : {false, true}) {
    CAPTURE(missing_table);
    Database database;
    SqlSession caller;
    caller_transaction = &caller;
    if (missing_table) {
      replies.emplace_back(namespace_not_found("oplog_history does not exist"));
    } else {
      database.rows({});
    }
    try {
      install_oplog_schema(caller);
      FAIL("migration attempted to wait on its own caller transaction");
    } catch (const TranslatorError& error) {
      const std::string message = error.what();
      CHECK(message.find("commit or roll back") != std::string::npos);
      CHECK(message.find("retry") != std::string::npos);
    }
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].kind == "query");
    CHECK(calls[0].session != &caller);
    CHECK(calls[0].statement == "SELECT id FROM chimera_meta.oplog_history WHERE id = 1");
    REQUIRE(transaction_probes.size() == 1);
    CHECK(transaction_probes[0] == &caller);
    CHECK(caller_transaction == &caller);
    CHECK(transaction_session == nullptr);
    CHECK(replies.empty());
  }
}

TEST_CASE("an initializer that won the readiness race is respected by the locked recheck") {
  Database database;
  SqlSession caller;
  database.rows({});         // not initialized at the first probe
  database.rows({{"1"}});    // another installer finished before the lock
  CHECK_NOTHROW(install_oplog_schema(caller));
  REQUIRE(calls.size() == 2);
  for (const auto& call : calls) {
    CHECK(call.kind == "query");
    CHECK(call.session != &caller);
    CHECK(call.statement == "SELECT id FROM chimera_meta.oplog_history WHERE id = 1");
  }
  REQUIRE(transaction_probes.size() == 1);
  CHECK(transaction_probes[0] == &caller);
  CHECK(replies.empty());
}

TEST_CASE("legacy empty oplog with a committed clock cannot publish a safe baseline") {
  Database database;
  SqlSession caller;
  database.rows({});              // history row absent
  database.rows({});              // still absent after acquiring schema lock
  database.rows({{"1"}});         // legacy oplog table exists
  database.rows({{"900", "7"}});  // committed event clock
  database.rows({});              // but its event history is entirely gone
  CHECK_THROWS_AS(install_oplog_schema(caller), TranslatorError);
  CHECK(matching("INSERT IGNORE INTO chimera_meta.oplog_history").empty());
  CHECK(matching("COMMIT").empty());
  CHECK(matching("ROLLBACK").size() == 1);
  for (const auto& call : calls) CHECK(call.session != &caller);
  CHECK(replies.empty());
}
