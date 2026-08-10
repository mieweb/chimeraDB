#pragma once

#include <string>
#include <string_view>

namespace chimera {

// The first word of a statement, upper-cased, with leading whitespace and
// comments removed. `--`, `#` and `/* … */` all hide a keyword from a naive
// check, which is exactly how such checks get walked past.
std::string leading_keyword(std::string_view statement);

// True when a statement only reads. The list is a whitelist on purpose: an
// unfamiliar keyword is refused rather than assumed harmless.
//
// This is one of two defences, not the only one — the gateway also runs the
// statement inside a read-only transaction. The keyword check exists because a
// read-only transaction does *not* stop DDL: `CREATE`/`DROP` commit implicitly
// before they run, so the server never sees them as part of the transaction.
bool is_read_only_statement(std::string_view statement);

}  // namespace chimera
