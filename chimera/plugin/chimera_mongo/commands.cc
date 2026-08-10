#include "commands.h"

#include <sys/time.h>

#include <algorithm>
#include <chrono>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/pipeline.h"
#include "chimera/project.h"
#include "chimera/sort.h"
#include "collection.h"
#include "cursor.h"
#include "oplog.h"
#include "sqlgateway.h"

namespace chimera {
namespace {

constexpr int32_t kMaxBsonObjectSize = 16 * 1024 * 1024;

// Wire version 17 is MongoDB 6.0. It is the lowest version that advertises
// everything Meteor's driver requires — logical sessions, retryable writes and
// the `hello` command — without promising 7.0/8.0 features we do not implement.
constexpr int32_t kMaxWireVersion = 17;
constexpr int32_t kMinWireVersion = 0;

// MongoDB's default first batch. Drivers override it; the shell does not.
constexpr int32_t kDefaultBatchSize = 101;

// How long an `awaitData` getMore parks before returning empty, and how often it
// re-checks while parked. The poll exists only because writes from an ordinary
// SQL client happen outside this process and cannot signal it directly.
constexpr int64_t kDefaultAwaitMs = 1000;
constexpr int64_t kOplogPollMs = 50;

int64_t now_millis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

std::optional<Value> field(const bson_t* doc, const char* name) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, doc, name)) return std::nullopt;
  return Value(*bson_iter_value(&it));
}

// A read-only view over a nested document or array. The bytes stay owned by the
// parent, so the view must not outlive it.
bool as_view(const bson_value_t& value, bson_t* out) {
  if (value.value_type != BSON_TYPE_DOCUMENT && value.value_type != BSON_TYPE_ARRAY) {
    return false;
  }
  return bson_init_static(out, value.value.v_doc.data, value.value.v_doc.data_len);
}

// Views a named sub-document *without* copying it. `field()` hands back an
// owning Value, and a view taken from a temporary one dangles the moment the
// expression ends — so anything that outlives a statement comes through here.
bool view_field(const bson_t* body, const char* name, bson_t* out) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, body, name)) return false;
  const uint8_t* data = nullptr;
  uint32_t length = 0;
  if (BSON_ITER_HOLDS_DOCUMENT(&it)) {
    bson_iter_document(&it, &length, &data);
  } else if (BSON_ITER_HOLDS_ARRAY(&it)) {
    bson_iter_array(&it, &length, &data);
  } else {
    return false;
  }
  return bson_init_static(out, data, length);
}

bool flag(const bson_t* doc, const char* name, bool fallback = false) {
  auto v = field(doc, name);
  if (!v) return fallback;
  if (v->type() == BSON_TYPE_BOOL) return v->get().value.v_bool;
  if (value_is_numeric(v->get())) return value_as_double(v->get()) != 0;
  return fallback;
}

int64_t number(const bson_t* doc, const char* name, int64_t fallback) {
  auto v = field(doc, name);
  if (!v || !value_is_numeric(v->get())) return fallback;
  return static_cast<int64_t>(value_as_double(v->get()));
}

std::string text_field(const bson_t* doc, const char* name) {
  auto v = field(doc, name);
  if (!v || v->type() != BSON_TYPE_UTF8) return {};
  return std::string(v->get().value.v_utf8.str, v->get().value.v_utf8.len);
}

void append_indexed(bson_t* array, uint32_t index, const bson_t* doc) {
  const char* key = nullptr;
  char buffer[16];
  const size_t length = bson_uint32_to_string(index, &key, buffer, sizeof buffer);
  bson_append_document(array, key, static_cast<int>(length), doc);
}

void append_indexed_int64(bson_t* array, uint32_t index, int64_t value) {
  const char* key = nullptr;
  char buffer[16];
  const size_t length = bson_uint32_to_string(index, &key, buffer, sizeof buffer);
  bson_append_int64(array, key, static_cast<int>(length), value);
}

void append_empty_array(bson_t* doc, const char* name) {
  bson_t array;
  bson_append_array_begin(doc, name, -1, &array);
  bson_append_array_end(doc, &array);
}

Bson ok_reply() {
  Bson reply;
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson error_reply(int code, const char* code_name, const std::string& message) {
  Bson reply;
  bson_t* b = reply.get();
  BSON_APPEND_DOUBLE(b, "ok", 0);
  BSON_APPEND_UTF8(b, "errmsg", message.c_str());
  BSON_APPEND_INT32(b, "code", code);
  BSON_APPEND_UTF8(b, "codeName", code_name);
  return reply;
}

Bson hello_reply(const ServerIdentity& identity, int64_t connection_id) {
  Bson reply;
  bson_t* b = reply.get();
  BSON_APPEND_BOOL(b, "helloOk", true);
  // `ismaster` is the legacy spelling; drivers still handshaking over OP_QUERY
  // read that one, newer ones read `isWritablePrimary`. Send both.
  BSON_APPEND_BOOL(b, "ismaster", true);
  BSON_APPEND_BOOL(b, "isWritablePrimary", true);

  // Meteor tails an oplog, and a driver only looks for one on a replica set.
  // ChimeraDB therefore presents itself as a single-node set (D7).
  BSON_APPEND_UTF8(b, "setName", "chimera");
  BSON_APPEND_INT32(b, "setVersion", 1);
  BSON_APPEND_BOOL(b, "secondary", false);

  bson_t hosts;
  BSON_APPEND_ARRAY_BEGIN(b, "hosts", &hosts);
  BSON_APPEND_UTF8(&hosts, "0", identity.host.c_str());
  bson_append_array_end(b, &hosts);
  BSON_APPEND_UTF8(b, "primary", identity.host.c_str());
  BSON_APPEND_UTF8(b, "me", identity.host.c_str());

  bson_t topology;
  BSON_APPEND_DOCUMENT_BEGIN(b, "topologyVersion", &topology);
  BSON_APPEND_OID(&topology, "processId", &identity.process_id);
  BSON_APPEND_INT64(&topology, "counter", 0);
  bson_append_document_end(b, &topology);

  BSON_APPEND_INT32(b, "maxBsonObjectSize", kMaxBsonObjectSize);
  BSON_APPEND_INT32(b, "maxMessageSizeBytes", wire::kMaxMessageBytes);
  BSON_APPEND_INT32(b, "maxWriteBatchSize", 100000);
  BSON_APPEND_DATE_TIME(b, "localTime", now_millis());
  BSON_APPEND_INT32(b, "logicalSessionTimeoutMinutes", 30);
  BSON_APPEND_INT32(b, "connectionId", static_cast<int32_t>(connection_id));
  BSON_APPEND_INT32(b, "minWireVersion", kMinWireVersion);
  BSON_APPEND_INT32(b, "maxWireVersion", kMaxWireVersion);
  BSON_APPEND_BOOL(b, "readOnly", false);
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

Bson build_info_reply() {
  Bson reply;
  bson_t* b = reply.get();
  // The version string is what drivers gate their feature checks on, so it has
  // to match the wire version we advertise.
  BSON_APPEND_UTF8(b, "version", "6.0.0");
  bson_t array;
  BSON_APPEND_ARRAY_BEGIN(b, "versionArray", &array);
  BSON_APPEND_INT32(&array, "0", 6);
  BSON_APPEND_INT32(&array, "1", 0);
  BSON_APPEND_INT32(&array, "2", 0);
  BSON_APPEND_INT32(&array, "3", 0);
  bson_append_array_end(b, &array);
  BSON_APPEND_UTF8(b, "gitVersion", "chimera");
  BSON_APPEND_UTF8(b, "sysInfo", "ChimeraDB on MariaDB");
  BSON_APPEND_INT32(b, "bits", 64);
  BSON_APPEND_BOOL(b, "debug", false);
  BSON_APPEND_INT32(b, "maxBsonObjectSize", kMaxBsonObjectSize);
  append_empty_array(b, "modules");
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

// A driver asks the replica set for its status to decide whether an oplog is
// worth tailing. ChimeraDB is a set of one that is always primary, so the answer
// is short and never changes — but it has to be *an* answer, or Meteor falls
// back to poll-and-diff and the oplog exists for nothing.
Bson repl_set_status_reply(const ServerIdentity& identity) {
  Bson reply;
  bson_t* b = reply.get();
  BSON_APPEND_UTF8(b, "set", "chimera");
  BSON_APPEND_DATE_TIME(b, "date", now_millis());
  BSON_APPEND_INT32(b, "myState", 1);  // PRIMARY
  BSON_APPEND_INT32(b, "term", 1);

  bson_t members;
  bson_t member;
  BSON_APPEND_ARRAY_BEGIN(b, "members", &members);
  BSON_APPEND_DOCUMENT_BEGIN(&members, "0", &member);
  BSON_APPEND_INT32(&member, "_id", 0);
  BSON_APPEND_UTF8(&member, "name", identity.host.c_str());
  BSON_APPEND_INT32(&member, "health", 1);
  BSON_APPEND_INT32(&member, "state", 1);
  BSON_APPEND_UTF8(&member, "stateStr", "PRIMARY");
  BSON_APPEND_BOOL(&member, "self", true);
  bson_append_document_end(&members, &member);
  bson_append_array_end(b, &members);

  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

// `getParameter` is a diagnostic surface with hundreds of knobs and ChimeraDB
// has none of them. Only the one drivers actually gate behaviour on is answered;
// anything else is left out of the reply rather than invented.
Bson get_parameter_reply(const bson_t* body) {
  Bson reply;
  bson_t* b = reply.get();
  if (bson_has_field(body, "featureCompatibilityVersion")) {
    bson_t fcv;
    BSON_APPEND_DOCUMENT_BEGIN(b, "featureCompatibilityVersion", &fcv);
    BSON_APPEND_UTF8(&fcv, "version", "6.0");
    bson_append_document_end(b, &fcv);
  }
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

// Everything one command handler needs. `body` outlives every view taken from it.
struct Ctx {
  const bson_t* body;
  const std::string& db;
  const std::string& argument;  // the command field's value, usually a collection
  ConnectionState& state;

  SqlSession& sql() { return state.sql(); }
  Namespace ns() const { return parse_namespace(db, argument); }
};

// Views the named sub-document, or an empty document when the field is absent,
// so an omitted filter/sort/projection needs no special case at every call site.
void sub_document(const Ctx& ctx, const char* name, bson_t* out) {
  if (!view_field(ctx.body, name, out)) bson_init(out);
}

// ---------------------------------------------------------------- reads

// Applies the parts of a query that BSON, not SQL, defines: cross-type ordering,
// paging and shape. See the M4 correction in chimeraDB-plan.md.
std::vector<Bson> shape_results(std::vector<Bson> documents, const bson_t* sort,
                                const bson_t* projection, int64_t skip, int64_t limit) {
  if (!bson_empty(sort)) sort_documents(documents, sort);

  if (skip > 0) {
    const size_t drop = std::min<size_t>(static_cast<size_t>(skip), documents.size());
    documents.erase(documents.begin(), documents.begin() + drop);
  }
  // A negative limit means "one batch, then close" — the magnitude still caps.
  const int64_t cap = limit < 0 ? -limit : limit;
  if (cap > 0 && documents.size() > static_cast<size_t>(cap)) {
    documents.resize(static_cast<size_t>(cap));
  }

  if (projection != nullptr && !bson_empty(projection)) {
    for (auto& document : documents) document = project(document.get(), projection);
  }
  return documents;
}

Bson cursor_reply(int64_t cursor_id, const std::string& ns, const char* batch_name,
                  const std::vector<Bson>& batch) {
  Bson reply;
  bson_t* b = reply.get();
  bson_t cursor;
  BSON_APPEND_DOCUMENT_BEGIN(b, "cursor", &cursor);
  BSON_APPEND_INT64(&cursor, "id", cursor_id);
  BSON_APPEND_UTF8(&cursor, "ns", ns.c_str());
  bson_t array;
  bson_append_array_begin(&cursor, batch_name, -1, &array);
  for (uint32_t i = 0; i < batch.size(); ++i) append_indexed(&array, i, batch[i].get());
  bson_append_array_end(&cursor, &array);
  bson_append_document_end(b, &cursor);
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

// `local.oplog.rs` is not a table but a view over chimera_meta.oplog. Meteor
// reads it twice: once with `{$natural: -1}` and limit 1 to learn the current
// head, then tailing from there.
Bson find_oplog(Ctx& ctx, const Namespace& ns) {
  install_oplog_schema(ctx.sql());

  bson_t filter;
  bson_t sort;
  sub_document(ctx, "filter", &filter);
  sub_document(ctx, "sort", &sort);

  const bool newest_first = number(&sort, "$natural", 1) < 0;
  const int64_t limit = number(ctx.body, "limit", 0);
  const int64_t batch_size = number(ctx.body, "batchSize", kDefaultBatchSize);
  const uint64_t wanted = limit > 0 ? static_cast<uint64_t>(limit)
                                    : static_cast<uint64_t>(std::max<int64_t>(batch_size, 1));

  // Captured before the read: every row up to here has now been considered,
  // including the ones the filter rejected, so a tail need never revisit them.
  const uint64_t head = oplog_head(ctx.sql());
  OplogBatch batch = read_oplog(ctx.sql(), &filter, 0, wanted, newest_first);

  if (!flag(ctx.body, "tailable")) {
    return cursor_reply(0, ns.text(), "firstBatch", batch.documents);
  }

  TailState tail;
  tail.filter = Bson::copy_of(&filter);
  tail.after_seq =
      (!newest_first && batch.documents.size() >= wanted) ? batch.last_seq : head;
  tail.await_data = flag(ctx.body, "awaitData");
  const int64_t cursor_id = CursorRegistry::instance().open_tail(ns.text(), std::move(tail));
  return cursor_reply(cursor_id, ns.text(), "firstBatch", batch.documents);
}

// A tailing getMore re-reads rather than draining a buffer, and parks when it
// catches up so the client is not left polling.
Bson tail_batch(Ctx& ctx, int64_t cursor_id, const Namespace& ns, const TailState& tail,
                int32_t batch_size, int64_t max_time_ms) {
  const uint64_t wanted = static_cast<uint64_t>(std::max(batch_size, 1));
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max<int64_t>(max_time_ms, 0));

  OplogBatch batch;
  uint64_t next_after = tail.after_seq;
  for (;;) {
    const uint64_t head = oplog_head(ctx.sql());
    batch = read_oplog(ctx.sql(), tail.filter.get(), tail.after_seq, wanted, false);
    next_after = batch.documents.size() >= wanted ? batch.last_seq : head;
    if (!batch.documents.empty() || !tail.await_data) break;

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    const int64_t left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    // A write from a plain SQL client never reaches this process, so the wait is
    // capped: writes over the wire wake it at once, external ones within a poll.
    wait_for_oplog_write(static_cast<uint64_t>(std::min<int64_t>(left, kOplogPollMs)));
  }

  CursorRegistry::instance().advance_tail(cursor_id, next_after);
  return cursor_reply(cursor_id, ns.text(), "nextBatch", batch.documents);
}

Bson cmd_find(Ctx& ctx) {
  const Namespace ns = ctx.ns();
  if (is_oplog_namespace(ns)) return find_oplog(ctx, ns);
  Collection collection(ctx.sql(), ns);

  std::vector<Bson> documents;
  // Reading a collection that was never created is empty, not an error — Mongo
  // creates collections lazily and clients rely on that.
  if (collection.exists()) {
    bson_t filter;
    bson_t sort;
    bson_t projection;
    sub_document(ctx, "filter", &filter);
    sub_document(ctx, "sort", &sort);
    sub_document(ctx, "projection", &projection);
    documents = shape_results(collection.load(&filter), &sort, &projection,
                              number(ctx.body, "skip", 0), number(ctx.body, "limit", 0));
  }

  const int64_t batch_size = number(ctx.body, "batchSize", kDefaultBatchSize);
  const bool single_batch = flag(ctx.body, "singleBatch") || number(ctx.body, "limit", 0) < 0;

  const size_t take = std::min<size_t>(
      documents.size(), batch_size < 0 ? 0 : static_cast<size_t>(batch_size));
  std::vector<Bson> first;
  first.reserve(take);
  for (size_t i = 0; i < take; ++i) first.push_back(std::move(documents[i]));
  documents.erase(documents.begin(), documents.begin() + take);

  const int64_t cursor_id =
      single_batch ? 0 : CursorRegistry::instance().open(ns.text(), std::move(documents));
  return cursor_reply(cursor_id, ns.text(), "firstBatch", first);
}

Bson cmd_get_more(Ctx& ctx) {
  auto id = field(ctx.body, "getMore");
  if (!id || !value_is_numeric(id->get())) {
    throw type_mismatch("getMore requires the cursor id as a 64-bit integer");
  }
  const int64_t cursor_id = static_cast<int64_t>(value_as_double(id->get()));
  const Namespace ns = parse_namespace(ctx.db, text_field(ctx.body, "collection"));
  const int32_t batch_size =
      static_cast<int32_t>(number(ctx.body, "batchSize", kDefaultBatchSize));

  TailState tail;
  if (CursorRegistry::instance().tail_state(cursor_id, ns.text(), &tail)) {
    return tail_batch(ctx, cursor_id, ns, tail, batch_size,
                      number(ctx.body, "maxTimeMS", kDefaultAwaitMs));
  }

  bool exhausted = false;
  std::vector<Bson> batch = CursorRegistry::instance().next_batch(
      cursor_id, ns.text(), batch_size, &exhausted);
  return cursor_reply(exhausted ? 0 : cursor_id, ns.text(), "nextBatch", batch);
}

Bson cmd_kill_cursors(Ctx& ctx) {
  auto cursors = field(ctx.body, "cursors");
  std::vector<int64_t> killed;
  std::vector<int64_t> not_found;
  if (cursors && cursors->type() == BSON_TYPE_ARRAY) {
    for (const auto& value : array_values(cursors->get())) {
      if (!value_is_numeric(value.get())) continue;
      const int64_t id = static_cast<int64_t>(value_as_double(value.get()));
      (CursorRegistry::instance().kill(id) ? killed : not_found).push_back(id);
    }
  }

  Bson reply;
  bson_t* b = reply.get();
  bson_t array;
  BSON_APPEND_ARRAY_BEGIN(b, "cursorsKilled", &array);
  for (uint32_t i = 0; i < killed.size(); ++i) append_indexed_int64(&array, i, killed[i]);
  bson_append_array_end(b, &array);
  BSON_APPEND_ARRAY_BEGIN(b, "cursorsNotFound", &array);
  for (uint32_t i = 0; i < not_found.size(); ++i) append_indexed_int64(&array, i, not_found[i]);
  bson_append_array_end(b, &array);
  append_empty_array(b, "cursorsAlive");
  append_empty_array(b, "cursorsUnknown");
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

Bson cmd_count(Ctx& ctx) {
  bson_t query;
  sub_document(ctx, "query", &query);
  Collection collection(ctx.sql(), ctx.ns());
  int64_t n = static_cast<int64_t>(collection.count(&query));
  n = std::max<int64_t>(0, n - number(ctx.body, "skip", 0));
  const int64_t limit = number(ctx.body, "limit", 0);
  if (limit > 0) n = std::min(n, limit);

  Bson reply;
  BSON_APPEND_INT32(reply.get(), "n", static_cast<int32_t>(n));
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

// `{$sql: "SELECT …"}` is a source stage: it produces the documents the rest of
// the pipeline reshapes, so it only makes sense first — there is nothing for it
// to read if a stage ran before it. Filtering belongs in its WHERE clause, which
// is the entire reason to reach for it, so the stages after it are the
// post-filter set and `$match` is not among them.
std::vector<Bson> take_sql_source(Ctx& ctx, std::vector<Bson>& stages) {
  if (stages.empty()) return {};
  const auto [name, body] = stage_operator(stages.front().get());
  if (name != "$sql") return {};
  if (body.type() != BSON_TYPE_UTF8) throw type_mismatch("$sql takes a statement string");
  const std::string statement(body.get().value.v_utf8.str, body.get().value.v_utf8.len);
  stages.erase(stages.begin());
  return run_sql_gateway(ctx.sql(), statement).documents;
}

Bson cmd_aggregate(Ctx& ctx) {
  bson_t pipeline;
  sub_document(ctx, "pipeline", &pipeline);
  std::vector<Bson> stages = pipeline_stages(&pipeline);

  std::vector<Bson> documents;
  std::string ns_text;
  if (!stages.empty() && stage_operator(stages.front().get()).first == "$sql") {
    // A `$sql` pipeline never touches a collection, so it runs against the
    // database rather than a namespace and the cursor is named accordingly.
    documents = run_stages(take_sql_source(ctx, stages), stages);
    ns_text = ctx.db + ".$cmd.aggregate";
  } else {
    Pipeline plan = split_pipeline(std::move(stages));
    const Namespace ns = ctx.ns();
    ns_text = ns.text();
    Collection collection(ctx.sql(), ns);
    // As with `find`, a collection that was never created aggregates to nothing.
    // The stages still run: `$count` and a `$group` on a constant key have
    // answers over an empty input, and a client is entitled to them.
    documents = run_stages(
        collection.exists() ? collection.load(plan.prefilter.get()) : std::vector<Bson>{},
        plan.stages);
  }

  const int64_t batch_size = number(ctx.body, "batchSize", kDefaultBatchSize);
  const size_t take =
      std::min<size_t>(documents.size(), batch_size < 0 ? 0 : static_cast<size_t>(batch_size));
  std::vector<Bson> first;
  first.reserve(take);
  for (size_t i = 0; i < take; ++i) first.push_back(std::move(documents[i]));
  documents.erase(documents.begin(), documents.begin() + take);

  const int64_t cursor_id =
      documents.empty() ? 0 : CursorRegistry::instance().open(ns_text, std::move(documents));
  return cursor_reply(cursor_id, ns_text, "firstBatch", first);
}

// The plain form: `db.runCommand({chimeraSql: "SELECT …"})`. Rows come back in a
// cursor like any other read, so a driver needs no special handling; a statement
// that returned no result set reports how many rows it changed instead.
Bson cmd_chimera_sql(Ctx& ctx) {
  SqlGatewayResult result = run_sql_gateway(ctx.sql(), ctx.argument);
  if (!result.had_result_set) {
    Bson reply;
    BSON_APPEND_INT64(reply.get(), "n", static_cast<int64_t>(result.affected_rows));
    BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
    return reply;
  }
  return cursor_reply(0, ctx.db + ".$cmd.chimeraSql", "firstBatch", result.documents);
}

Bson cmd_distinct(Ctx& ctx) {
  const std::string key = text_field(ctx.body, "key");
  if (key.empty()) throw missing_command_field("BSON field 'distinct.key' is missing but a required field");
  bson_t query;
  sub_document(ctx, "query", &query);

  Collection collection(ctx.sql(), ctx.ns());
  std::vector<Value> values;
  auto remember = [&values](const Value& candidate) {
    for (const auto& seen : values) {
      if (value_equal(seen.get(), candidate.get())) return;
    }
    values.push_back(candidate);
  };

  if (collection.exists()) {
    const Path path = split_path(key);
    for (const auto& document : collection.load(&query)) {
      auto value = path_get(document.get(), path);
      if (!value) continue;
      // An array field contributes its elements, not itself — that is what makes
      // distinct useful over Meteor's tag lists.
      if (value->type() == BSON_TYPE_ARRAY) {
        for (const auto& element : array_values(value->get())) remember(element);
      } else {
        remember(*value);
      }
    }
  }

  Bson reply;
  bson_t array;
  BSON_APPEND_ARRAY_BEGIN(reply.get(), "values", &array);
  for (uint32_t i = 0; i < values.size(); ++i) {
    const char* name = nullptr;
    char buffer[16];
    const size_t length = bson_uint32_to_string(i, &name, buffer, sizeof buffer);
    bson_append_value(&array, name, static_cast<int>(length), &values[i].get());
  }
  bson_append_array_end(reply.get(), &array);
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

// ---------------------------------------------------------------- writes

void append_write_error(bson_t* array, uint32_t slot, size_t op_index,
                        const TranslatorError& error) {
  bson_t entry;
  const char* key = nullptr;
  char buffer[16];
  const size_t length = bson_uint32_to_string(slot, &key, buffer, sizeof buffer);
  bson_append_document_begin(array, key, static_cast<int>(length), &entry);
  BSON_APPEND_INT32(&entry, "index", static_cast<int32_t>(op_index));
  BSON_APPEND_INT32(&entry, "code", error.code());
  BSON_APPEND_UTF8(&entry, "errmsg", error.what());
  bson_append_document_end(array, &entry);
}

// MongoDB omits `writeErrors` entirely when a batch succeeded, and the Node
// driver relies on that: it treats the field's presence as proof there is a
// first error to throw, and dies on an empty array before it can report
// anything useful. So the array is built to one side and attached only if it
// has something in it.
void append_write_errors(bson_t* reply, const bson_t* errors) {
  if (bson_empty(errors)) return;
  bson_append_array(reply, "writeErrors", 11, errors);
}

// Each element of a write batch is its own transaction. Mongo does not make a
// batch atomic, and rolling the whole batch back would discard the writes that
// already succeeded before an unordered failure.
template <typename Fn>
void run_atomically(SqlSession& sql, Fn&& body) {
  sql.begin();
  try {
    body();
  } catch (...) {
    sql.rollback();
    throw;
  }
  sql.commit();
  // The triggers appended oplog rows inside that transaction; now that it is
  // durable, any cursor parked on `awaitData` can be woken.
  signal_oplog_write();
}

Bson cmd_insert(Ctx& ctx) {
  auto documents = field(ctx.body, "documents");
  if (!documents || documents->type() != BSON_TYPE_ARRAY) {
    throw failed_to_parse("insert requires a 'documents' array");
  }
  const bool ordered = flag(ctx.body, "ordered", true);

  Collection collection(ctx.sql(), ctx.ns());
  collection.create(/*error_if_exists=*/false);  // Mongo creates on first insert

  Bson reply;
  Bson error_list;
  bson_t& errors = *error_list.get();
  int32_t inserted = 0;
  uint32_t slot = 0;
  const auto values = array_values(documents->get());
  for (size_t i = 0; i < values.size(); ++i) {
    bson_t document;
    if (!as_view(values[i].get(), &document)) {
      append_write_error(&errors, slot++, i,
                         type_mismatch("documents[] entries must be objects"));
      if (ordered) break;
      continue;
    }
    try {
      run_atomically(ctx.sql(), [&] { collection.insert(&document); });
      inserted++;
    } catch (const TranslatorError& e) {
      append_write_error(&errors, slot++, i, e);
      if (ordered) break;
    }
  }
  append_write_errors(reply.get(), error_list.get());
  BSON_APPEND_INT32(reply.get(), "n", inserted);
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_update(Ctx& ctx) {
  auto updates = field(ctx.body, "updates");
  if (!updates || updates->type() != BSON_TYPE_ARRAY) {
    throw failed_to_parse("update requires an 'updates' array");
  }
  const bool ordered = flag(ctx.body, "ordered", true);

  Collection collection(ctx.sql(), ctx.ns());
  collection.create(/*error_if_exists=*/false);

  int32_t matched = 0;
  int32_t modified = 0;
  std::vector<std::pair<size_t, Value>> upserted;

  Bson reply;
  Bson error_list;
  bson_t& errors = *error_list.get();
  uint32_t slot = 0;
  const auto values = array_values(updates->get());
  for (size_t i = 0; i < values.size(); ++i) {
    bson_t spec;
    if (!as_view(values[i].get(), &spec)) {
      append_write_error(&errors, slot++, i, type_mismatch("updates[] entries must be objects"));
      if (ordered) break;
      continue;
    }
    try {
      bson_t filter;
      bson_t update;
      if (!view_field(&spec, "q", &filter)) throw failed_to_parse("update.q must be an object");
      if (!view_field(&spec, "u", &update)) throw failed_to_parse("update.u must be an object");

      UpdateOutcome outcome;
      run_atomically(ctx.sql(), [&] {
        outcome = collection.update(&filter, &update, flag(&spec, "multi"), flag(&spec, "upsert"));
      });
      matched += static_cast<int32_t>(outcome.matched);
      modified += static_cast<int32_t>(outcome.modified);
      if (outcome.upserted) upserted.emplace_back(i, outcome.upserted_id);
    } catch (const TranslatorError& e) {
      append_write_error(&errors, slot++, i, e);
      if (ordered) break;
    }
  }
  append_write_errors(reply.get(), error_list.get());

  // An upsert counts toward `n`, which is how a driver learns it happened.
  BSON_APPEND_INT32(reply.get(), "n", matched + static_cast<int32_t>(upserted.size()));
  BSON_APPEND_INT32(reply.get(), "nModified", modified);
  if (!upserted.empty()) {
    bson_t array;
    BSON_APPEND_ARRAY_BEGIN(reply.get(), "upserted", &array);
    for (uint32_t i = 0; i < upserted.size(); ++i) {
      bson_t entry;
      const char* key = nullptr;
      char buffer[16];
      const size_t length = bson_uint32_to_string(i, &key, buffer, sizeof buffer);
      bson_append_document_begin(&array, key, static_cast<int>(length), &entry);
      BSON_APPEND_INT32(&entry, "index", static_cast<int32_t>(upserted[i].first));
      bson_append_value(&entry, "_id", 3, &upserted[i].second.get());
      bson_append_document_end(&array, &entry);
    }
    bson_append_array_end(reply.get(), &array);
  }
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_delete(Ctx& ctx) {
  auto deletes = field(ctx.body, "deletes");
  if (!deletes || deletes->type() != BSON_TYPE_ARRAY) {
    throw failed_to_parse("delete requires a 'deletes' array");
  }
  const bool ordered = flag(ctx.body, "ordered", true);

  Collection collection(ctx.sql(), ctx.ns());
  int32_t removed = 0;

  Bson reply;
  Bson error_list;
  bson_t& errors = *error_list.get();
  uint32_t slot = 0;
  if (collection.exists()) {
    const auto values = array_values(deletes->get());
    for (size_t i = 0; i < values.size(); ++i) {
      bson_t spec;
      if (!as_view(values[i].get(), &spec)) {
        append_write_error(&errors, slot++, i,
                           type_mismatch("deletes[] entries must be objects"));
        if (ordered) break;
        continue;
      }
      try {
        bson_t filter;
        if (!view_field(&spec, "q", &filter)) throw failed_to_parse("delete.q must be an object");
        // `limit` is 0 for "every match" and 1 for "just one" — no other value.
        const bool just_one = number(&spec, "limit", 0) == 1;
        uint64_t n = 0;
        run_atomically(ctx.sql(), [&] { n = collection.remove(&filter, just_one); });
        removed += static_cast<int32_t>(n);
      } catch (const TranslatorError& e) {
        append_write_error(&errors, slot++, i, e);
        if (ordered) break;
      }
    }
  }
  append_write_errors(reply.get(), error_list.get());
  BSON_APPEND_INT32(reply.get(), "n", removed);
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_find_and_modify(Ctx& ctx) {
  bson_t query;
  bson_t sort;
  bson_t fields;
  sub_document(ctx, "query", &query);
  sub_document(ctx, "sort", &sort);
  sub_document(ctx, "fields", &fields);
  const bool remove = flag(ctx.body, "remove");
  const bool return_new = flag(ctx.body, "new");
  const bool upsert = flag(ctx.body, "upsert");

  bson_t update_view;
  const bool has_update = view_field(ctx.body, "update", &update_view);
  if (!remove && !has_update) {
    throw failed_to_parse("findAndModify requires either 'remove' or an 'update' object");
  }

  Collection collection(ctx.sql(), ctx.ns());
  if (!remove) collection.create(/*error_if_exists=*/false);

  Bson value;  // the document to report back
  bool have_value = false;
  int32_t n = 0;
  bool updated_existing = false;
  Value upserted_id;
  bool did_upsert = false;

  run_atomically(ctx.sql(), [&] {
    std::vector<Bson> matches;
    if (collection.exists()) {
      matches = shape_results(collection.load(&query), &sort, nullptr, 0, 1);
    }

    if (!matches.empty()) {
      n = 1;
      auto id = path_get(matches[0].get(), {"_id"});
      if (!id) throw internal_error("stored document has no _id");
      // Re-select by _id so the write lands on exactly the document that sorted
      // first, not on whatever else the filter also matched.
      Bson by_id;
      bson_append_value(by_id.get(), "_id", 3, &id->get());

      if (remove) {
        value = Bson::copy_of(matches[0].get());
        have_value = true;
        collection.remove(by_id.get(), true);
      } else {
        updated_existing = true;
        collection.update(by_id.get(), &update_view, false, false);
        if (return_new) {
          auto refreshed = collection.load(by_id.get());
          if (!refreshed.empty()) {
            value = std::move(refreshed[0]);
            have_value = true;
          }
        } else {
          value = Bson::copy_of(matches[0].get());
          have_value = true;
        }
      }
    } else if (!remove && upsert) {
      UpdateOutcome outcome = collection.update(&query, &update_view, false, true);
      did_upsert = outcome.upserted;
      upserted_id = outcome.upserted_id;
      if (return_new && outcome.upserted) {
        Bson by_id;
        bson_append_value(by_id.get(), "_id", 3, &outcome.upserted_id.get());
        auto refreshed = collection.load(by_id.get());
        if (!refreshed.empty()) {
          value = std::move(refreshed[0]);
          have_value = true;
        }
      }
    }
  });

  Bson reply;
  bson_t* b = reply.get();
  bson_t last_error;
  BSON_APPEND_DOCUMENT_BEGIN(b, "lastErrorObject", &last_error);
  BSON_APPEND_INT32(&last_error, "n", did_upsert ? 1 : n);
  BSON_APPEND_BOOL(&last_error, "updatedExisting", updated_existing);
  if (did_upsert) bson_append_value(&last_error, "upserted", 8, &upserted_id.get());
  bson_append_document_end(b, &last_error);
  if (have_value) {
    Bson shaped = bson_empty(&fields) ? Bson::copy_of(value.get()) : project(value.get(), &fields);
    BSON_APPEND_DOCUMENT(b, "value", shaped.get());
  } else {
    BSON_APPEND_NULL(b, "value");
  }
  BSON_APPEND_DOUBLE(b, "ok", 1);
  return reply;
}

// ---------------------------------------------------------------- namespaces

Bson cmd_create(Ctx& ctx) {
  // Creating a collection that already exists is a no-op on the oracle, so long
  // as the options match — and we accept no options yet.
  Collection(ctx.sql(), ctx.ns()).create(/*error_if_exists=*/false);
  return ok_reply();
}

Bson cmd_drop(Ctx& ctx) {
  const Namespace ns = ctx.ns();
  Collection collection(ctx.sql(), ns);
  // Dropping what is not there still succeeds, and reports no indexes.
  const auto indexes = collection.exists() ? collection.list_indexes() : std::vector<IndexSpec>{};
  collection.drop();

  Bson reply;
  BSON_APPEND_INT32(reply.get(), "nIndexesWas", static_cast<int32_t>(indexes.size()));
  BSON_APPEND_UTF8(reply.get(), "ns", ns.text().c_str());
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_drop_database(Ctx& ctx) {
  Collection::bootstrap(ctx.sql());
  drop_database(ctx.sql(), ctx.db);
  Bson reply;
  BSON_APPEND_UTF8(reply.get(), "dropped", ctx.db.c_str());
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_list_databases(Ctx& ctx) {
  Collection::bootstrap(ctx.sql());
  Bson reply;
  bson_t array;
  BSON_APPEND_ARRAY_BEGIN(reply.get(), "databases", &array);
  uint32_t slot = 0;
  for (const auto& name : list_databases(ctx.sql())) {
    Bson entry;
    BSON_APPEND_UTF8(entry.get(), "name", name.c_str());
    BSON_APPEND_INT64(entry.get(), "sizeOnDisk", 0);
    BSON_APPEND_BOOL(entry.get(), "empty", false);
    append_indexed(&array, slot++, entry.get());
  }
  bson_append_array_end(reply.get(), &array);
  BSON_APPEND_INT64(reply.get(), "totalSize", 0);
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_list_collections(Ctx& ctx) {
  Collection::bootstrap(ctx.sql());
  const bool name_only = flag(ctx.body, "nameOnly");
  std::vector<Bson> batch;
  for (const auto& name : list_collections(ctx.sql(), ctx.db)) {
    Bson entry;
    BSON_APPEND_UTF8(entry.get(), "name", name.c_str());
    BSON_APPEND_UTF8(entry.get(), "type", "collection");
    if (!name_only) {
      bson_t options;
      BSON_APPEND_DOCUMENT_BEGIN(entry.get(), "options", &options);
      bson_append_document_end(entry.get(), &options);
      bson_t info;
      BSON_APPEND_DOCUMENT_BEGIN(entry.get(), "info", &info);
      BSON_APPEND_BOOL(&info, "readOnly", false);
      bson_append_document_end(entry.get(), &info);
    }
    batch.push_back(std::move(entry));
  }
  return cursor_reply(0, ctx.db + ".$cmd.listCollections", "firstBatch", batch);
}

Bson index_document(const IndexSpec& spec) {
  Bson entry;
  BSON_APPEND_INT32(entry.get(), "v", 2);
  bson_t keys;
  BSON_APPEND_DOCUMENT_BEGIN(entry.get(), "key", &keys);
  for (const auto& key : spec.keys) {
    bson_append_int32(&keys, key.first.c_str(), static_cast<int>(key.first.size()), key.second);
  }
  bson_append_document_end(entry.get(), &keys);
  BSON_APPEND_UTF8(entry.get(), "name", spec.name.c_str());
  if (spec.unique) BSON_APPEND_BOOL(entry.get(), "unique", true);
  return entry;
}

Bson cmd_list_indexes(Ctx& ctx) {
  const Namespace ns = ctx.ns();
  std::vector<Bson> batch;
  for (const auto& spec : Collection(ctx.sql(), ns).list_indexes()) {
    batch.push_back(index_document(spec));
  }
  return cursor_reply(0, ns.text(), "firstBatch", batch);
}

// MongoDB's default index name is every "path_direction" pair joined by "_".
Bson cmd_create_indexes(Ctx& ctx) {
  auto indexes = field(ctx.body, "indexes");
  if (!indexes || indexes->type() != BSON_TYPE_ARRAY) {
    throw failed_to_parse("createIndexes requires an 'indexes' array");
  }
  Collection collection(ctx.sql(), ctx.ns());
  const bool created_collection = !collection.exists();
  collection.create(/*error_if_exists=*/false);

  const int32_t before = static_cast<int32_t>(collection.list_indexes().size());
  for (const auto& value : array_values(indexes->get())) {
    bson_t entry;
    if (!as_view(value.get(), &entry)) throw type_mismatch("indexes[] entries must be objects");
    bson_t key_view;
    if (!view_field(&entry, "key", &key_view)) throw failed_to_parse("an index needs a 'key'");

    IndexSpec spec;
    spec.unique = flag(&entry, "unique");
    for (const auto& member : document_fields(&key_view)) {
      if (!value_is_numeric(member.second.get())) {
        throw not_implemented("only ascending and descending index keys are supported");
      }
      spec.keys.emplace_back(member.first,
                             value_as_double(member.second.get()) < 0 ? -1 : 1);
    }
    spec.name = text_field(&entry, "name");
    // The server does not invent a name; clients send one. (MongoDB used to
    // derive `sku_1_bin_1`-style names and no longer does.)
    if (spec.name.empty()) throw failed_to_parse("an index needs a 'name'");
    collection.create_index(spec);
  }
  const int32_t after = static_cast<int32_t>(collection.list_indexes().size());

  Bson reply;
  BSON_APPEND_BOOL(reply.get(), "createdCollectionAutomatically", created_collection);
  BSON_APPEND_INT32(reply.get(), "numIndexesBefore", before);
  BSON_APPEND_INT32(reply.get(), "numIndexesAfter", after);
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

Bson cmd_drop_indexes(Ctx& ctx) {
  Collection collection(ctx.sql(), ctx.ns());
  const auto before = collection.list_indexes();

  const std::string named = text_field(ctx.body, "index");
  if (named == "*") {
    // The _id index is the InnoDB primary key and cannot be dropped, which is
    // also true of MongoDB.
    for (const auto& spec : before) {
      if (spec.name != "_id_") collection.drop_index(spec.name);
    }
  } else if (!named.empty()) {
    if (!collection.drop_index(named)) {
      throw index_not_found("index not found with name [" + named + "]");
    }
  } else {
    throw not_implemented("dropIndexes by key document is not supported; name the index");
  }

  Bson reply;
  BSON_APPEND_INT32(reply.get(), "nIndexesWas", static_cast<int32_t>(before.size()));
  BSON_APPEND_DOUBLE(reply.get(), "ok", 1);
  return reply;
}

}  // namespace

ConnectionState::ConnectionState(const ServerIdentity& identity, int64_t connection_id)
    : identity_(identity), connection_id_(connection_id) {}

SqlSession& ConnectionState::sql() {
  if (!sql_) sql_ = std::make_unique<SqlSession>();
  return *sql_;
}

Bson dispatch_command(const wire::Request& req, ConnectionState& state) noexcept {
  try {
    auto fields = document_fields(req.body.get());
    if (fields.empty()) return error_reply(9, "FailedToParse", "command document is empty");

    // MongoDB identifies a command by the *first* field of the document.
    const std::string& command = fields.front().first;
    const bson_value_t& argument_value = fields.front().second.get();
    const std::string argument =
        argument_value.value_type == BSON_TYPE_UTF8
            ? std::string(argument_value.value.v_utf8.str, argument_value.value.v_utf8.len)
            : std::string();

    if (command == "hello" || command == "isMaster" || command == "ismaster") {
      return hello_reply(state.identity(), state.connection_id());
    }
    if (command == "ping") return ok_reply();
    if (command == "buildInfo" || command == "buildinfo") return build_info_reply();
    if (command == "replSetGetStatus") return repl_set_status_reply(state.identity());
    if (command == "getParameter") return get_parameter_reply(req.body.get());
    // Logical sessions are accepted and ignored: `lsid` and `txnNumber` ride
    // along on every command and need no bookkeeping while there is no
    // multi-statement transaction to resume.
    if (command == "endSessions" || command == "refreshSessions") return ok_reply();

    Ctx ctx{req.body.get(), req.database, argument, state};

    if (command == "find") return cmd_find(ctx);
    if (command == "getMore") return cmd_get_more(ctx);
    if (command == "killCursors") return cmd_kill_cursors(ctx);
    if (command == "count") return cmd_count(ctx);
    if (command == "aggregate") return cmd_aggregate(ctx);
    if (command == "distinct") return cmd_distinct(ctx);
    if (command == "chimeraSql") return cmd_chimera_sql(ctx);

    if (command == "insert") return cmd_insert(ctx);
    if (command == "update") return cmd_update(ctx);
    if (command == "delete") return cmd_delete(ctx);
    if (command == "findAndModify" || command == "findandmodify") {
      return cmd_find_and_modify(ctx);
    }

    if (command == "create") return cmd_create(ctx);
    if (command == "drop") return cmd_drop(ctx);
    if (command == "dropDatabase") return cmd_drop_database(ctx);
    if (command == "listDatabases") return cmd_list_databases(ctx);
    if (command == "listCollections") return cmd_list_collections(ctx);
    if (command == "listIndexes") return cmd_list_indexes(ctx);
    if (command == "createIndexes") return cmd_create_indexes(ctx);
    if (command == "dropIndexes") return cmd_drop_indexes(ctx);

    return error_reply(59, "CommandNotFound", "no such command: '" + command + "'");
  } catch (const TranslatorError& e) {
    return error_reply(e.code(), e.code_name().c_str(), e.what());
  } catch (const std::exception& e) {
    return error_reply(1, "InternalError", e.what());
  } catch (...) {
    return error_reply(1, "InternalError", "unknown failure while handling command");
  }
}

}  // namespace chimera
