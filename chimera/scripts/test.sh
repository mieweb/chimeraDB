#!/usr/bin/env bash
# The whole test pyramid for one server version. CI runs it twice (10.11, 11.8).
#
#   test.sh --server 10.11|11.8
#
# Layers that need no server (hygiene, translator unit tests) run first and fail
# fast — there is no point starting a mariadbd to discover a compile error.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"

note "=== hygiene (repo-wide) ==="
"$CHIMERA_DIR/scripts/check-hygiene.sh"

note "=== translator unit tests (server-independent) ==="
"$CHIMERA_DIR/scripts/build-translator.sh"

note "=== building the Mongo head for $SERVER_VERSION ==="
"$CHIMERA_DIR/scripts/build-plugin.sh" --server "$SERVER_VERSION"

# Restart so the tests exercise the plugin that was just built, not whatever
# happened to be loaded before.
"$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION" >/dev/null
"$CHIMERA_DIR/scripts/run-server.sh" --server "$SERVER_VERSION" >/dev/null

note "=== SQL layer on $SERVER_VERSION ==="
chimera_require_running
"$CHIMERA_DIR/scripts/probe-json.sh" --server "$SERVER_VERSION"
"$CHIMERA_DIR/scripts/demo-m1.sh" --server "$SERVER_VERSION"

note "=== Mongo wire protocol on $SERVER_VERSION ==="
"$CHIMERA_DIR/scripts/demo-m3.sh" --server "$SERVER_VERSION"

# The referee: every command surface is replayed against a real mongod and the
# two transcripts must be identical.
note "=== differential vs MongoDB on $SERVER_VERSION ==="
"$CHIMERA_DIR/tests/differential/run.sh" --server "$SERVER_VERSION"

note "all layers green on $SERVER_VERSION"
