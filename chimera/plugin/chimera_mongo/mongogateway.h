#pragma once

#include <string>

namespace chimera {

// Mongo for clients that only speak SQL — the other direction from sqlgateway.h.
// Runs one pasted shell statement against `db` and returns its result as
// Extended JSON: an array for `find`/`aggregate`, a document or `null` for
// `findOne`, a number for `countDocuments`, and the command's own reply for a
// write.
//
// The statement is turned into an ordinary command document and dispatched
// through the same handlers the wire protocol uses, so a write from here takes
// the same locks and produces the same oplog entry as a write from a driver.
std::string run_mongo_gateway(const std::string& db, const std::string& statement);

}  // namespace chimera
