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

// AUTO_INCREMENT gaps are not evidence of lost history: a rolled-back writer
// consumes a sequence without committing an event. Only committed pruning may
// advance pruned_*. The separate legacy boundary conservatively describes the
// unknown prefix of an oplog created before this metadata existed.
const char kHistoryTable[] =
    "CREATE TABLE IF NOT EXISTS chimera_meta.oplog_history ("
    " id TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
    " pruned_seq BIGINT UNSIGNED NOT NULL,"
    " pruned_ts_t INT UNSIGNED NOT NULL,"
    " pruned_ts_i INT UNSIGNED NOT NULL,"
    " legacy_baseline TINYINT UNSIGNED NOT NULL,"
    " baseline_seq BIGINT UNSIGNED NOT NULL,"
    " baseline_ts_t INT UNSIGNED NOT NULL,"
    " baseline_ts_i INT UNSIGNED NOT NULL) ENGINE=InnoDB";

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
std::mutex g_schema_mutex;
std::mutex g_prune_mutex;
std::condition_variable g_wait_cv;
uint64_t g_write_generation = 0;

// How often the pruner wakes. Not a knob: the limits are the policy, and this
// is only how coarsely it is enforced.
constexpr int kPruneIntervalSeconds = 10;

}  // namespace

void install_oplog_schema(SqlSession& caller) {
  // All DDL belongs to its own session, even when a Mongo connection has an
  // explicit chimeraSql transaction open. Serialize freshness detection with
  // creation: a concurrent installer must not misclassify a fresh oplog as a
  // legacy one. A crash before seeding metadata takes the conservative path.
  SqlSession sql;
  const auto ready = [&sql] {
    try {
      const ResultSet rows = sql.query("SELECT id FROM chimera_meta.oplog_history WHERE id = 1");
      return rows.rows.size() == 1 && rows.rows[0].size() == 1 && rows.rows[0][0].has_value();
    } catch (const TranslatorError& error) {
      if (error.code() != 26) throw;
      return false;
    }
  };
  if (ready()) return;
  // A legacy caller may already hold the clock or a procedure metadata lock.
  // Waiting for migration would then wait for this very command to complete.
  // Check BEFORE the mutex: its owner may itself be waiting on that caller.
  if (caller.in_transaction()) {
    throw bad_value("oplog history initialization is required; commit or roll back "
                    "the current SQL transaction, then retry");
  }
  std::lock_guard<std::mutex> guard(g_schema_mutex);
  if (ready()) return;
  const ResultSet existing = sql.query(
      "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'chimera_meta'"
      " AND table_name = 'oplog'");
  if (existing.rows.size() != 1 || existing.rows[0].size() != 1 || !existing.rows[0][0]) {
    throw internal_error("could not determine whether the oplog already exists");
  }
  const bool legacy = *existing.rows[0][0] != "0";
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
  sql.exec(kHistoryTable);
  sql.exec("INSERT IGNORE INTO chimera_meta.oplog_clock (id, ts_t, ts_i) VALUES (1, 0, 0)");
  sql.exec(kAppendProcedure);
  sql.exec(kAdoptProcedure);
  // Writers hold the clock lock until commit. Seed the migration boundary from
  // one committed snapshot, never from the next AUTO_INCREMENT value. The old
  // pruner always retained its newest row. A nonzero clock with no rows means
  // history was removed outside that invariant; do not silently bless it.
  sql.begin();
  try {
    const ResultSet clock = sql.query(
        "SELECT ts_t, ts_i FROM chimera_meta.oplog_clock WHERE id = 1 FOR UPDATE");
    if (clock.rows.size() != 1 || clock.rows[0].size() != 2 ||
        !clock.rows[0][0] || !clock.rows[0][1]) {
      throw internal_error("oplog clock metadata is missing");
    }
    if (legacy && (*clock.rows[0][0] != "0" || *clock.rows[0][1] != "0") &&
        sql.query("SELECT seq FROM chimera_meta.oplog ORDER BY seq LIMIT 1").rows.empty()) {
      throw internal_error("cannot establish history for an empty legacy oplog with a nonzero clock");
    }
    sql.exec(
        "INSERT IGNORE INTO chimera_meta.oplog_history "
        "(id, pruned_seq, pruned_ts_t, pruned_ts_i, legacy_baseline, baseline_seq, "
        "baseline_ts_t, baseline_ts_i) SELECT 1, 0, 0, 0, " +
        std::string(legacy ? "IF(COALESCE(oldest.seq, 0) > 1, 1, 0)" : "0") +
        ", " + (legacy ? "COALESCE(oldest.seq - 1, 0), COALESCE(oldest.ts_t, 0), "
                          "COALESCE(oldest.ts_i, 0)"
                        : "0, 0, 0") +
        " FROM (SELECT 1) AS singleton LEFT JOIN "
        "(SELECT seq, ts_t, ts_i FROM chimera_meta.oplog ORDER BY seq LIMIT 1) AS oldest ON 1 = 1");
    sql.commit();
  } catch (...) {
    sql.rollback();
    throw;
  }
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
  if (max_rows == 0 && max_age_seconds == 0) return 0;
  // Only pruning removes oplog rows; trigger writers append immutable events
  // above the committed head. Serialize pruning passes without blocking those
  // writers, and do all survivor/candidate scans in autocommit before taking
  // the clock lock. In particular, OFFSET max_rows must not stall every writer
  // for a walk over the default 100,000 survivors on each no-op pass.
  std::lock_guard<std::mutex> pruning(g_prune_mutex);
  const uint64_t head = oplog_head(sql);
  if (head == 0) return 0;
  std::string condition;
  if (max_age_seconds > 0) {
    const ResultSet now = sql.query("SELECT UNIX_TIMESTAMP()");
    if (now.rows.size() != 1 || now.rows[0].size() != 1 || !now.rows[0][0]) {
      throw internal_error("could not determine the oplog retention cutoff");
    }
    const uint64_t seconds = std::strtoull(now.rows[0][0]->c_str(), nullptr, 10);
    if (seconds > max_age_seconds) {
      condition = "ts_t < " + std::to_string(seconds - max_age_seconds);
    }
  }
  if (max_rows > 0) {
    // Count actual retained rows, not sequence distance: rollbacks can leave
    // arbitrarily large gaps between neighboring committed events.
    const ResultSet cut = sql.query(
        "SELECT seq FROM chimera_meta.oplog ORDER BY seq DESC LIMIT 1 OFFSET " +
        std::to_string(max_rows - 1));
    if (!cut.rows.empty() && cut.rows[0][0]) {
      if (!condition.empty()) condition += " OR ";
      condition += "seq < " + *cut.rows[0][0];
    }
  }
  if (condition.empty()) return 0;
  // Freeze both the head and age cutoff. Commits during planning only add rows
  // beyond this range; they can leave the cap temporarily exceeded until the
  // next pass, but cannot change the selected events or make us over-prune.
  const std::string where = "seq < " + std::to_string(head) + " AND (" + condition + ")";
  const ResultSet newest = sql.query(
      "SELECT seq FROM chimera_meta.oplog WHERE " + where + " ORDER BY seq DESC LIMIT 1");
  if (newest.rows.empty()) return 0;
  if (newest.rows.size() != 1 || newest.rows[0].size() != 1 || !newest.rows[0][0]) {
    throw internal_error("could not determine the last event selected for pruning");
  }
  const ResultSet timestamp = sql.query(
      "SELECT ts_t, ts_i FROM chimera_meta.oplog WHERE " + where +
      " ORDER BY ts_t DESC, ts_i DESC LIMIT 1");
  if (timestamp.rows.size() != 1 || timestamp.rows[0].size() != 2 ||
      !timestamp.rows[0][0] || !timestamp.rows[0][1]) {
    throw internal_error("could not determine the timestamp of pruned history");
  }

  uint64_t removed = 0;
  sql.begin();
  try {
    // Only constant-size metadata/primary-key checks and the actual deletion
    // remain while writers are blocked. No candidate-discovery scan runs under
    // this lock. Deletion and its two watermarks still commit together.
    const ResultSet clock = sql.query("SELECT id FROM chimera_meta.oplog_clock WHERE id = 1 FOR UPDATE");
    if (clock.rows.size() != 1 || clock.rows[0].size() != 1 || !clock.rows[0][0]) {
      throw internal_error("oplog clock metadata is missing");
    }
    ResultSet history = sql.query(
        "SELECT pruned_seq FROM chimera_meta.oplog_history WHERE id = 1 FOR UPDATE");
    if (history.rows.size() != 1 || history.rows[0].size() != 1 || !history.rows[0][0]) {
      throw internal_error("oplog history metadata is missing");
    }
    const ResultSet candidate = sql.query(
        "SELECT seq FROM chimera_meta.oplog WHERE seq = " + *newest.rows[0][0] +
        " AND (" + where + ")");
    if (candidate.rows.empty()) {
      // Unexpected external maintenance changed the private oplog. Replan on
      // a later pass instead of publishing history for an unverified range.
      sql.commit();
      return 0;
    }
    if (candidate.rows.size() != 1 || candidate.rows[0].size() != 1 ||
        candidate.rows[0][0] != newest.rows[0][0]) {
      throw internal_error("the planned oplog pruning boundary could not be verified");
    }
    // Bound the DELETE by the last selected event too, so its range cannot
    // walk the retained tail while checking the age/count disjunction.
    sql.exec("DELETE FROM chimera_meta.oplog WHERE seq <= " + *newest.rows[0][0] +
             " AND (" + where + ")");
    removed = sql.affected_rows();
    if (removed > 0) {
      const std::string t = *timestamp.rows[0][0], i = *timestamp.rows[0][1];
      // Assign the increment first: MariaDB evaluates assignments left to
      // right, and both comparisons must see the previous seconds value.
      sql.exec("UPDATE chimera_meta.oplog_history SET pruned_seq = GREATEST(pruned_seq, " +
               *newest.rows[0][0] + "), pruned_ts_i = CASE WHEN pruned_ts_t < " + t +
               " THEN " + i + " WHEN pruned_ts_t = " + t +
               " THEN GREATEST(pruned_ts_i, " + i + ") ELSE pruned_ts_i END, "
               "pruned_ts_t = GREATEST(pruned_ts_t, " + t + ") WHERE id = 1");
    }
    sql.commit();
  } catch (...) {
    sql.rollback();
    throw;
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
