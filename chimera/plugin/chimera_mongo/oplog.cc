#include "oplog.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/filter.h"

namespace chimera {

const char kOplogDb[] = "local";
const char kOplogCollection[] = "oplog.rs";

bool is_oplog_namespace(const Namespace& ns) {
  return ns.db == kOplogDb && ns.collection == kOplogCollection;
}

namespace {

// The plugin's own connection is the server's internal anonymous user, so
// anything it creates would be owned by ''@'' — an account that does not exist.
// A trigger with a missing definer refuses to fire, which would break exactly
// the case that matters: a write from an ordinary `mariadb` client.
//
// So the oplog machinery gets one owner of its own. It is locked, so it is a
// name to hang privileges on rather than a way in.
const char kDefiner[] = "'chimera'@'localhost'";

// Every oplog entry is stamped under the same row lock, so `seq` order, ts
// order, and commit order are the same order. That single-row lock is also what
// serializes concurrent writers — the price of a totally ordered oplog, and the
// same trade a real single-node replica set makes.
const char kClockTable[] =
    "CREATE TABLE IF NOT EXISTS chimera_meta.oplog_clock ("
    " id TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
    " ts_t INT UNSIGNED NOT NULL,"
    " ts_i INT UNSIGNED NOT NULL) ENGINE=InnoDB";

const char kOplogTable[] =
    "CREATE TABLE IF NOT EXISTS chimera_meta.oplog ("
    " seq BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
    " ts_t INT UNSIGNED NOT NULL,"
    " ts_i INT UNSIGNED NOT NULL,"
    " op ENUM('i','u','d') NOT NULL,"
    " ns VARCHAR(512) NOT NULL,"
    " o JSON NOT NULL,"
    " o2 JSON NULL,"
    " KEY ts (ts_t, ts_i)) ENGINE=InnoDB";

// `ts_i` is a per-second counter derived under the clock row's lock, which is
// what makes (ts_t, ts_i) unique and monotonic without a second round trip.
const char kAppendProcedure[] =
    "CREATE OR REPLACE DEFINER='chimera'@'localhost' PROCEDURE chimera_meta.oplog_append("
    " IN p_op CHAR(1), IN p_ns VARCHAR(512), IN p_o JSON, IN p_o2 JSON)"
    " MODIFIES SQL DATA"
    " BEGIN"
    "  UPDATE chimera_meta.oplog_clock"
    "     SET ts_i = IF(ts_t = UNIX_TIMESTAMP(), ts_i + 1, 1),"
    "         ts_t = UNIX_TIMESTAMP()"
    "   WHERE id = 1;"
    "  INSERT INTO chimera_meta.oplog (ts_t, ts_i, op, ns, o, o2)"
    "  SELECT ts_t, ts_i, p_op, p_ns, p_o, p_o2"
    "    FROM chimera_meta.oplog_clock WHERE id = 1;"
    " END";

// Adoption is the *only* place trigger DDL lives. `create` calls it over the
// wire and a DBA calls it by hand for a table that predates ChimeraDB (D7), so
// neither path can drift from the other.
//
// It assembles DDL by concatenation, so it whitelists identifiers first — the
// same character set the wire side enforces, re-checked here because a DBA can
// call this directly with anything. It runs as the *caller*, so adopting a
// table still requires the caller's own rights over it; only the triggers it
// leaves behind belong to the locked owner account.
//
// 'u' entries carry the whole new document, which is the replacement style
// Meteor's oplog driver understands with no diff-application logic.
const char kAdoptProcedure[] =
    "CREATE OR REPLACE DEFINER='chimera'@'localhost' PROCEDURE chimera_meta.chimera_adopt_table("
    " IN p_db VARCHAR(64), IN p_coll VARCHAR(64))"
    " MODIFIES SQL DATA"
    " SQL SECURITY INVOKER"
    " COMMENT 'Mirror an existing ChimeraDB collection table into the oplog'"
    " BEGIN"
    "  DECLARE v_ns VARCHAR(512);"
    "  DECLARE v_tbl VARCHAR(200);"
    "  DECLARE v_stem VARCHAR(64);"
    "  IF p_db NOT REGEXP '^[A-Za-z0-9_$.-]+$' OR p_coll NOT REGEXP '^[A-Za-z0-9_$.-]+$' THEN"
    "   SIGNAL SQLSTATE '45000'"
    "    SET MESSAGE_TEXT = 'chimera_adopt_table: unsupported identifier';"
    "  END IF;"
    "  SET v_ns = CONCAT(p_db, '.', p_coll);"
    "  SET v_tbl = CONCAT('`', p_db, '`.`', p_coll, '`');"
    // Trigger names share the database namespace and cap at 64 bytes, so the
    // stem is a readable prefix plus a digest that keeps long names apart.
    "  SET v_stem = CONCAT(LEFT(p_coll, 40), '_', SUBSTR(MD5(p_coll), 1, 8));"
    "  INSERT IGNORE INTO chimera_meta.collections (db_name, coll_name) VALUES (p_db, p_coll);"
    "  SET @chimera_ddl = CONCAT('CREATE OR REPLACE DEFINER=''chimera''@''localhost''"
    " TRIGGER `', p_db, '`.`oplog_i_', v_stem,"
    "   '` AFTER INSERT ON ', v_tbl,"
    "   ' FOR EACH ROW CALL chimera_meta.oplog_append(''i'', ', QUOTE(v_ns), ', NEW.doc, NULL)');"
    "  PREPARE chimera_stmt FROM @chimera_ddl;"
    "  EXECUTE chimera_stmt;"
    "  DEALLOCATE PREPARE chimera_stmt;"
    "  SET @chimera_ddl = CONCAT('CREATE OR REPLACE DEFINER=''chimera''@''localhost''"
    " TRIGGER `', p_db, '`.`oplog_u_', v_stem,"
    "   '` AFTER UPDATE ON ', v_tbl,"
    "   ' FOR EACH ROW CALL chimera_meta.oplog_append(''u'', ', QUOTE(v_ns),"
    "   ', NEW.doc, JSON_OBJECT(''_id'', JSON_EXTRACT(NEW.doc, ''$._id'')))');"
    "  PREPARE chimera_stmt FROM @chimera_ddl;"
    "  EXECUTE chimera_stmt;"
    "  DEALLOCATE PREPARE chimera_stmt;"
    "  SET @chimera_ddl = CONCAT('CREATE OR REPLACE DEFINER=''chimera''@''localhost''"
    " TRIGGER `', p_db, '`.`oplog_d_', v_stem,"
    "   '` AFTER DELETE ON ', v_tbl,"
    "   ' FOR EACH ROW CALL chimera_meta.oplog_append(''d'', ', QUOTE(v_ns),"
    "   ', JSON_OBJECT(''_id'', JSON_EXTRACT(OLD.doc, ''$._id'')), NULL)');"
    "  PREPARE chimera_stmt FROM @chimera_ddl;"
    "  EXECUTE chimera_stmt;"
    "  DEALLOCATE PREPARE chimera_stmt;"
    " END";

// One row of chimera_meta.oplog as canonical extJSON text.
//
// Assembled with CONCAT rather than JSON_OBJECT because MariaDB's JSON type is
// text: JSON_OBJECT('o', o) would embed the document as a *string* instead of
// nesting it. The columns are integers, an enum, and JSON, so the only value
// needing escaping is `ns`, and JSON_QUOTE does that.
const char kEntryExpr[] =
    "CONCAT('{\"ts\":{\"$timestamp\":{\"t\":', ts_t, ',\"i\":', ts_i,"
    " '}},\"op\":\"', op, '\",\"ns\":', JSON_QUOTE(ns),"
    " ',\"o\":', o,"
    // A real oplog omits o2 on inserts and deletes rather than nulling it, and
    // Meteor tests for the field's presence.
    " IF(o2 IS NULL, '', CONCAT(',\"o2\":', o2)),"
    " ',\"v\":{\"$numberInt\":\"2\"}'"
    " ',\"wall\":{\"$date\":{\"$numberLong\":\"', ts_t * 1000, '\"}}}')";

std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
uint64_t g_write_generation = 0;

// How often the pruner wakes. Not a knob: the limits are the policy, and this
// is only how coarsely it is enforced.
constexpr int kPruneIntervalSeconds = 10;

}  // namespace

void install_oplog_schema(SqlSession& sql) {
  // The owner comes first: a procedure cannot name a definer that does not yet
  // exist. SELECT and TRIGGER are global because collections live in per-database
  // tables created on demand, and a trigger body reading NEW.doc is checked
  // against its definer. The account is locked and the only bodies that run as
  // it are the fixed ones below, so this is a name for privileges, not a login.
  sql.exec(std::string("CREATE USER IF NOT EXISTS ") + kDefiner + " ACCOUNT LOCK");
  sql.exec(std::string("GRANT SELECT, TRIGGER ON *.* TO ") + kDefiner);
  sql.exec(std::string("GRANT SELECT, INSERT, UPDATE, DELETE, EXECUTE ON chimera_meta.* TO ") +
           kDefiner);
  sql.exec("CREATE DATABASE IF NOT EXISTS chimera_meta");
  sql.exec(kClockTable);
  sql.exec(kOplogTable);
  sql.exec("INSERT IGNORE INTO chimera_meta.oplog_clock (id, ts_t, ts_i) VALUES (1, 0, 0)");
  sql.exec(kAppendProcedure);
  sql.exec(kAdoptProcedure);
}

void install_oplog_triggers(SqlSession& sql, const Namespace& ns) {
  install_oplog_schema(sql);
  sql.exec(sql.render("CALL chimera_meta.chimera_adopt_table(?, ?)",
                      {Param{ns.db}, Param{ns.collection}}));
}

OplogBatch read_oplog(SqlSession& sql, const bson_t* filter, uint64_t after_seq, uint64_t limit,
                      bool newest_first) {
  const SqlFilter compiled = compile_filter(filter, "doc");
  const std::string where = sql.render(compiled.sql, compiled.params);

  ResultSet rows = sql.query(
      "SELECT seq, doc FROM (SELECT seq, " + std::string(kEntryExpr) +
      " AS doc FROM chimera_meta.oplog WHERE seq > " + std::to_string(after_seq) +
      ") AS entries WHERE " + where + " ORDER BY seq " + (newest_first ? "DESC" : "ASC") +
      " LIMIT " + std::to_string(limit));

  OplogBatch batch;
  batch.last_seq = after_seq;
  for (const auto& row : rows.rows) {
    if (!row[0] || !row[1]) continue;
    batch.last_seq = std::strtoull(row[0]->c_str(), nullptr, 10);
    batch.documents.push_back(from_extjson(*row[1]));
  }
  return batch;
}

uint64_t oplog_head(SqlSession& sql) {
  ResultSet rows = sql.query("SELECT COALESCE(MAX(seq), 0) FROM chimera_meta.oplog");
  if (rows.rows.empty() || !rows.rows[0][0]) return 0;
  return std::strtoull(rows.rows[0][0]->c_str(), nullptr, 10);
}

uint64_t prune_oplog(SqlSession& sql, uint64_t max_rows, uint64_t max_age_seconds) {
  uint64_t removed = 0;
  if (max_age_seconds > 0) {
    sql.exec("DELETE FROM chimera_meta.oplog WHERE ts_t < UNIX_TIMESTAMP() - " +
             std::to_string(max_age_seconds));
    removed += sql.affected_rows();
  }
  if (max_rows > 0) {
    // Delete by sequence rather than with LIMIT so the cut point is computed
    // once and the delete stays a single range scan on the primary key.
    ResultSet rows = sql.query(
        "SELECT COALESCE(MAX(seq), 0) - " + std::to_string(max_rows) +
        " FROM chimera_meta.oplog");
    if (!rows.rows.empty() && rows.rows[0][0]) {
      const long long cut = std::strtoll(rows.rows[0][0]->c_str(), nullptr, 10);
      if (cut > 0) {
        sql.exec("DELETE FROM chimera_meta.oplog WHERE seq <= " + std::to_string(cut));
        removed += sql.affected_rows();
      }
    }
  }
  return removed;
}

bool wait_for_oplog_write(uint64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(g_wait_mutex);
  const uint64_t seen = g_write_generation;
  return g_wait_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return g_write_generation != seen; });
}

void signal_oplog_write() {
  {
    std::lock_guard<std::mutex> lock(g_wait_mutex);
    ++g_write_generation;
  }
  g_wait_cv.notify_all();
}

OplogPruner::OplogPruner(const unsigned long long* max_rows,
                         const unsigned long long* max_age_seconds)
    : max_rows_(max_rows), max_age_seconds_(max_age_seconds) {
  thread_ = std::thread([this] { run(); });
}

OplogPruner::~OplogPruner() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void OplogPruner::run() {
  SqlThreadScope thread_scope;

  // The first pass waits a full interval: plugin init runs while the server is
  // still coming up, and a local connection opened too early would block it.
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(kPruneIntervalSeconds), [this] { return stop_; });
      if (stop_) return;
    }
    try {
      SqlSession sql;
      install_oplog_schema(sql);
      prune_oplog(sql, *max_rows_, *max_age_seconds_);
    } catch (const std::exception& e) {
      // Pruning is housekeeping: a failure must never take the server with it,
      // and the next pass will try again.
      std::fprintf(stderr, "chimera_mongo: oplog pruning failed: %s\n", e.what());
    }
  }
}

}  // namespace chimera
