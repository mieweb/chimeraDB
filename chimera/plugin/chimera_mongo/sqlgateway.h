#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chimera/bson.h"
#include "sql.h"

namespace chimera {

// What one statement produced. A statement that returned no result set at all
// (an UPDATE, say) is not the same thing as one that returned no rows, and a
// caller can tell the difference.
struct SqlGatewayResult {
  bool had_result_set = false;
  std::vector<Bson> documents;
  uint64_t affected_rows = 0;
};

// SQL for clients that only speak Mongo: `{chimeraSql: "SELECT …"}` and the
// `{$sql: "SELECT …"}` aggregation stage both land here. One result row becomes
// one document, so a query answers into a driver's cursor without the caller
// needing to know a table was involved.
SqlGatewayResult run_sql_gateway(SqlSession& sql, const std::string& statement);

// The plugin entry point hands over the address of its system variable rather
// than its value, so `SET GLOBAL chimera_mongo_sql_writes = ON` takes effect on
// the next statement instead of at the next restart.
void set_sql_gateway_write_flag(const char* flag);

// Set once at plugin start: when the listener accepts non-loopback connections
// (chimera_mongo_insecure_bind), the gateway refuses every statement. An
// unauthenticated socket that can reach `{chimeraSql: …}` can read the whole
// server, not just collection tables — a wider blast radius than documents
// (https://github.com/mieweb/chimeraDB/issues/5, stage 1).
void set_sql_gateway_network_exposed(bool exposed);

}  // namespace chimera
