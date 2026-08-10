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

// True only for an IPv4 loopback address (all of 127.0.0.0/8); anything that
// does not parse as IPv4 is not loopback. The plugin refuses to start a
// non-loopback listener without an explicit opt-in — the wire has no
// authentication yet (issue #5) — and, like the statement checks above, the
// predicate lives here so a security control is unit-testable without a server.
bool is_loopback_address(std::string_view address);

}  // namespace chimera
