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

void enqueue_rows(std::vector<chimera::Row> rows) {
  chimera::ResultSet result;
  result.rows = std::move(rows);
  results.push_back(std::move(result));
}

void return_rows(std::vector<chimera::Row> rows) {
  results.clear();
  query_count = 0;
  schema_installs = 0;
  schema_install_session = nullptr;
  install_error.reset();
  enqueue_rows(std::move(rows));
}

void history_floor(uint64_t seq, bool has_events = true) {
  return_rows({{std::to_string(seq), has_events ? "1" : "0"}});
}

void time_boundary(uint64_t before, uint64_t pruned, uint32_t t, uint32_t i,
                   bool legacy = false, uint64_t baseline = 0,
                   uint32_t baseline_t = 0, uint32_t baseline_i = 0) {
  return_rows({{std::to_string(before), std::to_string(pruned), std::to_string(t),
                std::to_string(i), legacy ? "1" : "0", std::to_string(baseline),
                std::to_string(baseline_t), std::to_string(baseline_i)}});
}

chimera::ChangeStreamOptions at_time(uint32_t t, uint32_t i) {
  chimera::ChangeStreamOptions options;
  options.start = chimera::ChangeStreamStart::kOperationTime;
  options.ts_t = t;
  options.ts_i = i;
  return options;
}
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
  history_floor(0, false);
  CHECK_NOTHROW(require_change_stream_history(sql, 0));
  history_floor(0);
  CHECK_NOTHROW(require_change_stream_history(sql, 0));
  history_floor(2);
  try {
    require_change_stream_history(sql, 0);
    FAIL("the first events were pruned but the cold-start cursor was accepted");
  } catch (const TranslatorError& error) {
    CHECK(error.code() == 286);
    CHECK(error.code_name() == "ChangeStreamHistoryLost");
  }
}

TEST_CASE("resume boundaries use actual deleted events with exclusive token semantics") {
  SqlSession sql;
  history_floor(99);
  CHECK_THROWS_AS(require_change_stream_history(sql, 98), TranslatorError);
  history_floor(99);
  CHECK_NOTHROW(require_change_stream_history(sql, 99));
  history_floor(99);
  CHECK_NOTHROW(require_change_stream_history(sql, 100));
  history_floor(0, false);
  CHECK_THROWS_AS(require_change_stream_history(sql, 1), TranslatorError);
  history_floor(std::numeric_limits<uint64_t>::max());
  CHECK_NOTHROW(require_change_stream_history(sql, std::numeric_limits<uint64_t>::max()));
}

TEST_CASE("operation-time starts include the oldest retained event after pruning") {
  SqlSession sql;
  // The last actual deletion was seq 70. Rolled-back sequences 71..99 do not
  // move the boundary, even though the oldest surviving event is seq 100.
  time_boundary(0, 70, 1700000000, 6);
  const uint64_t after = resolve_change_stream_start(sql, at_time(1700000000, 7));
  REQUIRE(after == 70);
  history_floor(70);
  CHECK_NOTHROW(require_change_stream_history(sql, after));
  return_rows({{"100", "{}"}});
  enqueue_rows({{"70", "1"}});
  const OplogBatch batch = read_changestream(sql, {"test", "events"}, after, 100);
  CHECK(batch.documents.size() == 1);
  CHECK(batch.last_seq == 100);
}

TEST_CASE("operation times at or before a deleted event report history lost") {
  SqlSession sql;
  for (const auto& options : {at_time(1700000000, 7), at_time(1700000000, 6),
                              at_time(1699999999, UINT32_MAX)}) {
    time_boundary(0, 70, 1700000000, 7);
    try {
      resolve_change_stream_start(sql, options);
      FAIL("a time older than retained history was accepted");
    } catch (const TranslatorError& error) {
      CHECK(error.code() == 286);
    }
  }
}

TEST_CASE("rollback gaps before the first commit do not imply history loss") {
  SqlSession sql;
  for (const auto& options : {at_time(0, 0), at_time(1700000000, 6), at_time(1700000000, 7)}) {
    time_boundary(0, 0, 0, 0);
    CHECK(resolve_change_stream_start(sql, options) == 0);
  }
  return_rows({{"100", "{}"}});
  enqueue_rows({{"0", "1"}});
  const OplogBatch batch = read_changestream(sql, {"test", "events"}, 0, 100);
  REQUIRE(batch.documents.size() == 1);
  CHECK(batch.last_seq == 100);
  // A later time with a retained predecessor resumes after that predecessor.
  time_boundary(105, 0, 0, 0);
  CHECK(resolve_change_stream_start(sql, at_time(1700000001, 0)) == 105);
}

TEST_CASE("a safe time between deleted and retained events resumes at the deletion watermark") {
  SqlSession sql;
  time_boundary(0, 1, 1700000000, 7);
  const uint64_t after = resolve_change_stream_start(sql, at_time(1700000000, 8));
  REQUIRE(after == 1);
  return_rows({{"100", "{}"}});
  enqueue_rows({{"1", "1"}});
  CHECK(read_changestream(sql, {"test", "events"}, after, 100).last_seq == 100);
}

TEST_CASE("legacy migration keeps the retained boundary inclusive without claiming a deletion") {
  SqlSession sql;
  time_boundary(0, 0, 0, 0, true, 99, 1700000000, 7);
  CHECK(resolve_change_stream_start(sql, at_time(1700000000, 7)) == 99);
  time_boundary(0, 0, 0, 0, true, 99, 1700000000, 7);
  CHECK_THROWS_AS(resolve_change_stream_start(sql, at_time(1700000000, 6)), TranslatorError);
  // Once a retained event is actually pruned, equality becomes unsafe.
  time_boundary(0, 100, 1700000000, 7, true, 99, 1700000000, 7);
  CHECK_THROWS_AS(resolve_change_stream_start(sql, at_time(1700000000, 7)), TranslatorError);
}

TEST_CASE("a time origin on an empty oplog remains subject to cold-start pruning") {
  SqlSession sql;
  time_boundary(0, 0, 0, 0);
  const uint64_t after = resolve_change_stream_start(sql, at_time(0, 0));
  REQUIRE(after == 0);
  history_floor(0);
  CHECK_NOTHROW(require_change_stream_history(sql, after));
  history_floor(2);
  CHECK_THROWS_AS(require_change_stream_history(sql, after), TranslatorError);
}

TEST_CASE("a prune after time resolution cannot skip the first retained event") {
  SqlSession sql;
  time_boundary(0, 99, 1700000000, 6);
  const uint64_t after = resolve_change_stream_start(sql, at_time(1700000000, 7));
  REQUIRE(after == 99);
  // Row 100 disappears between resolution and the event SELECT; returning
  // row 101 successfully would silently skip the event the time names.
  return_rows({{"101", "{}"}});
  enqueue_rows({{"100", "1"}});
  CHECK_THROWS_AS(read_changestream(sql, {"test", "events"}, after, 100), TranslatorError);
  CHECK(query_count == 2);
}

TEST_CASE("each event SELECT validates history after reading even when no events match") {
  SqlSession sql;
  for (bool matching_event : {false, true}) {
    return_rows(matching_event ? std::vector<Row>{{"100", "{}"}} : std::vector<Row>{});
    // The event SELECT may have seen a complete page, but the subsequent
    // history snapshot proves pruning raced that read. Fail before delivery.
    enqueue_rows({{"101", "1"}});
    try {
      read_changestream(sql, {"test", "events"}, 99, 100);
      FAIL("the event read skipped its post-read history validation");
    } catch (const TranslatorError& error) {
      CHECK(error.code() == 286);
    }
    CHECK(query_count == 2);
  }
}

TEST_CASE("missing or NULL history metadata fails closed") {
  SqlSession sql;
  for (const auto& rows : {std::vector<Row>{}, std::vector<Row>{{std::nullopt}}}) {
    return_rows(rows);
    CHECK_THROWS_AS(require_change_stream_history(sql, 0), TranslatorError);
    return_rows(rows);
    CHECK_THROWS_AS(resolve_change_stream_start(sql, at_time(0, 0)), TranslatorError);
  }
  time_boundary(0, 0, 0, 0);
  auto& row = std::get<ResultSet>(results.front()).rows[0];
  row[1] = std::nullopt;
  CHECK_THROWS_AS(resolve_change_stream_start(sql, at_time(0, 0)), TranslatorError);
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
  enqueue_rows({{"1", "1"}});
  OplogBatch batch = read_changestream(sql, ns, 10, 100);
  CHECK(batch.last_seq == 10);
  CHECK(batch.documents.empty());
  return_rows({{"11", "{}"}, {"12", "{}"}});
  enqueue_rows({{"1", "1"}});
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
  // Forward the real caller so the installer can inspect its transaction
  // state. The real installer's separate DDL session is tested in the pruning
  // storage target; this target replaces that whole installer at link time.
  CHECK(schema_install_session == &sql);
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
