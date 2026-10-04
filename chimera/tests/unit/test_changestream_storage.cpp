// Test the storage-facing change-stream logic without starting MariaDB. The
// SQL adapter is replaced at link time; the real event decoding and history
// validation run against explicit server-result rows, including SQL NULL.
#include <doctest/doctest.h>

#include <deque>
#include <limits>
#include <utility>

#include "changestream.h"
#include "chimera/error.h"

namespace {
std::deque<chimera::ResultSet> results;

void return_rows(std::vector<chimera::Row> rows) {
  results.clear();
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
  ResultSet result = std::move(results.front());
  results.pop_front();
  return result;
}
std::string SqlSession::render(std::string_view sql, const std::vector<Param>&) const {
  return std::string(sql);
}
std::string Namespace::text() const { return db + "." + collection; }
uint64_t oplog_head(SqlSession&) { return 0; }
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
