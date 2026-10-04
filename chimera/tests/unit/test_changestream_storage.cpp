// Test the storage-facing change-stream logic without starting MariaDB. The
// SQL adapter is replaced at link time; the real event decoding and history
// validation run against explicit server-result rows, including SQL NULL.
#include <doctest/doctest.h>

#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

#include "changestream.h"
#include "chimera/error.h"

namespace {
std::deque<std::variant<chimera::ResultSet, chimera::TranslatorError>> results;
unsigned query_count = 0;
unsigned schema_installs = 0;
const chimera::SqlSession* schema_install_session = nullptr;
std::optional<chimera::TranslatorError> install_error;

void return_rows(std::vector<chimera::Row> rows) {
  results.clear();
  query_count = 0;
  schema_installs = 0;
  schema_install_session = nullptr;
  install_error.reset();
  chimera::ResultSet result;
  result.rows = std::move(rows);
  results.push_back(std::move(result));
}

void oldest(uint64_t seq) { return_rows({{std::to_string(seq)}}); }
}  // namespace

namespace chimera {
SqlSession::SqlSession() = default;
SqlSession::~SqlSession() = default;
ResultSet SqlSession::query(const std::string&) {
  REQUIRE_FALSE(results.empty());
  ++query_count;
  auto result = std::move(results.front());
  results.pop_front();
  if (const auto* error = std::get_if<TranslatorError>(&result)) throw *error;
  return std::move(std::get<ResultSet>(result));
}
std::string SqlSession::render(std::string_view sql, const std::vector<Param>&) const {
  return std::string(sql);
}
std::string Namespace::text() const { return db + "." + collection; }
uint64_t oplog_head(SqlSession&) { return 0; }
void install_oplog_schema(SqlSession& sql) {
  ++schema_installs;
  schema_install_session = &sql;
  if (install_error) throw *install_error;
}
}  // namespace chimera

using namespace chimera;

TEST_CASE("cold-start stream positions must detect subsequent pruning") {
  SqlSession sql;
  oldest(0);
  CHECK_NOTHROW(require_change_stream_history(sql, 0));
  oldest(1);
  CHECK_NOTHROW(require_change_stream_history(sql, 0));
  oldest(3);
  try {
    require_change_stream_history(sql, 0);
    FAIL("the first events were pruned but the cold-start cursor was accepted");
  } catch (const TranslatorError& error) {
    CHECK(error.code() == 286);
    CHECK(error.code_name() == "ChangeStreamHistoryLost");
  }
}

TEST_CASE("history boundaries accept retained positions and reject gaps") {
  SqlSession sql;
  oldest(100);
  CHECK_THROWS_AS(require_change_stream_history(sql, 98), TranslatorError);
  oldest(100);
  CHECK_NOTHROW(require_change_stream_history(sql, 99));
  oldest(100);
  CHECK_NOTHROW(require_change_stream_history(sql, 100));
  oldest(0);
  CHECK_THROWS_AS(require_change_stream_history(sql, 1), TranslatorError);
  oldest(std::numeric_limits<uint64_t>::max());
  CHECK_NOTHROW(require_change_stream_history(sql, std::numeric_limits<uint64_t>::max()));
}

TEST_CASE("a NULL change-stream row fails before it can be skipped") {
  SqlSession sql;
  const Namespace ns{"test", "events"};
  for (const Row& broken : {Row{std::nullopt, "{}"}, Row{"11", std::nullopt}}) {
    // A valid row after the bad one must not hide it by advancing last_seq.
    return_rows({broken, {"12", "{}"}});
    try {
      read_changestream(sql, ns, 10, 100);
      FAIL("a NULL row was silently skipped");
    } catch (const TranslatorError& error) {
      CHECK(error.code_name() == "InternalError");
      CHECK(std::string(error.what()).find("oplog row rendered NULL") != std::string::npos);
    }
  }
}

TEST_CASE("change-stream storage preserves empty and populated batch positions") {
  SqlSession sql;
  const Namespace ns{"test", "events"};
  return_rows({});
  OplogBatch batch = read_changestream(sql, ns, 10, 100);
  CHECK(batch.last_seq == 10);
  CHECK(batch.documents.empty());
  return_rows({{"11", "{}"}, {"12", "{}"}});
  batch = read_changestream(sql, ns, 10, 100);
  CHECK(batch.last_seq == 12);
  CHECK(batch.documents.size() == 2);
}

TEST_CASE("the first operation-time read bootstraps a missing clock and retries") {
  SqlSession sql;
  return_rows({{"0", "0"}});
  results.push_front(namespace_not_found("Table chimera_meta.oplog_clock does not exist"));
  const OperationTime now = current_operation_time_or_initialize(sql);
  CHECK(now.t == 0);
  CHECK(now.i == 0);
  CHECK(query_count == 2);
  CHECK(schema_installs == 1);
  CHECK(schema_install_session != nullptr);
  CHECK(schema_install_session != &sql);
  CHECK(results.empty());
}

TEST_CASE("an existing operation clock is read without schema DDL") {
  SqlSession sql;
  return_rows({{"1700000000", "7"}});
  const OperationTime now = current_operation_time_or_initialize(sql);
  CHECK(now.t == 1700000000);
  CHECK(now.i == 7);
  CHECK(query_count == 1);
  CHECK(schema_installs == 0);
}

TEST_CASE("operation-time bootstrap does not hide unrelated SQL errors") {
  SqlSession sql;
  return_rows({});
  results.front() = internal_error("clock storage read failed");
  CHECK_THROWS_WITH_AS(current_operation_time_or_initialize(sql),
                       "clock storage read failed", TranslatorError);
  CHECK(query_count == 1);
  CHECK(schema_installs == 0);
}

TEST_CASE("operation-time bootstrap propagates failed initialization and failed retries") {
  SqlSession sql;
  return_rows({{"0", "0"}});
  results.push_front(namespace_not_found("clock is missing"));
  install_error = internal_error("schema initialization denied");
  CHECK_THROWS_WITH_AS(current_operation_time_or_initialize(sql),
                       "schema initialization denied", TranslatorError);
  CHECK(query_count == 1);
  CHECK(schema_installs == 1);
  CHECK(results.size() == 1);

  return_rows({});
  results.front() = namespace_not_found("clock is still missing after initialization");
  results.push_front(namespace_not_found("clock is missing"));
  CHECK_THROWS_WITH_AS(current_operation_time_or_initialize(sql),
                       "clock is still missing after initialization", TranslatorError);
  CHECK(query_count == 2);
  CHECK(schema_installs == 1);
  CHECK(results.empty());
}

TEST_CASE("transactional operation-time reads never initialize missing schema") {
  SqlSession sql;
  return_rows({});
  results.front() = namespace_not_found("clock is missing inside a transaction");
  CHECK_THROWS_WITH_AS(current_operation_time(sql),
                       "clock is missing inside a transaction", TranslatorError);
  CHECK(query_count == 1);
  CHECK(schema_installs == 0);
}
