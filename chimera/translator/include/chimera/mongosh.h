#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "chimera/bson.h"

namespace chimera {

// One statement copied out of a mongo shell: `db.<collection>.<verb>(<args>)`.
// Nothing is interpreted here beyond the shape — which verb it is and what its
// arguments were — because the gateway turns it into an ordinary command
// document and lets the existing command handlers do the work.
struct ShellCall {
  std::string collection;
  std::string verb;
  std::vector<Value> args;
};

// Parses mongosh's *source* syntax, not JSON: keys may be unquoted, strings may
// use either quote, and `ObjectId('…')` is a value. That is the whole point —
// the statement is meant to survive a copy-paste out of a shell session, and a
// user who has to rewrite it into strict JSON first has not been given a
// gateway, only a chore.
ShellCall parse_shell_call(std::string_view statement);

// What the caller should pull out of the reply once the command has run.
enum class ShellResult {
  Documents,      // every document in the cursor's first batch
  FirstDocument,  // findOne: the first document, or null
  Count,          // the reply's `n`
  Reply,          // the reply document itself
};

struct ShellCommand {
  Bson command;
  ShellResult result = ShellResult::Reply;
};

// Turns a parsed statement into an ordinary command document. A shell verb and
// a driver's call therefore reach the same handler, take the same locks and
// write the same oplog entry — the gateway adds a spelling, not a code path.
ShellCommand build_shell_command(const ShellCall& call);

}  // namespace chimera
