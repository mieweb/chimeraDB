#!/usr/bin/env bash
# Milestone 3: prove the Mongo head is really speaking the wire protocol, using
# the real MongoDB shell as the client. The shell is a black-box reference — if it
# is satisfied, a driver will be too.
#
#   demo-m3.sh --server 10.11|11.8

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"
chimera_require_running

[[ -x $REFERENCE_MONGO ]] || die "no mongo shell at $REFERENCE_MONGO"

# Runs one JS expression through the shell and prints the result on one line.
mongo_eval() {
  "$REFERENCE_MONGO" --port "$MONGO_PORT" --quiet --eval "$1" 2>&1 | tr -d ' \n'
}

note "1. the shell completes a handshake and answers ping ($SERVER_VERSION, port $MONGO_PORT)"
check_eq "ping ok" "$(mongo_eval 'db.runCommand({ping:1}).ok')" "1"

note "2. hello presents a single-node replica set, which is what oplog tailing needs"
check_eq "isWritablePrimary" "$(mongo_eval 'db.hello().isWritablePrimary')" "true"
check_eq "setName" "$(mongo_eval 'db.hello().setName')" "chimera"
check_eq "me" "$(mongo_eval 'db.hello().me')" "127.0.0.1:$MONGO_PORT"
check_eq "hosts[0]" "$(mongo_eval 'db.hello().hosts[0]')" "127.0.0.1:$MONGO_PORT"
check_eq "maxWireVersion" "$(mongo_eval 'db.hello().maxWireVersion')" "17"
check_eq "minWireVersion" "$(mongo_eval 'db.hello().minWireVersion')" "0"
check_eq "logicalSessionTimeoutMinutes" \
  "$(mongo_eval 'db.hello().logicalSessionTimeoutMinutes')" "30"

note "3. the legacy OP_QUERY handshake works too — drivers open with it"
# isMaster over the legacy path is what the shell sends before it knows we
# speak OP_MSG; getting here at all proves OP_REPLY framing is right.
check_eq "isMaster ok" "$(mongo_eval 'db.runCommand({isMaster:1}).ok')" "1"

note "4. buildInfo reports a version consistent with the wire version"
# The release core is what drivers gate on; the -chimera-<version> suffix exists
# so mongosh names what it is really talking to (see build_info_reply).
check_eq "buildInfo version" \
  "$(mongo_eval 'db.runCommand({buildInfo:1}).version.split("-")[0]')" "6.0.0"
check_eq "buildInfo names chimera" \
  "$(mongo_eval 'db.runCommand({buildInfo:1}).version.split("-")[1]')" "chimera"

note "5. sessions are accepted and ignored"
check_eq "endSessions ok" "$(mongo_eval 'db.runCommand({endSessions:[]}).ok')" "1"

note "6. an unknown command returns a proper error envelope, not a dropped connection"
check_eq "unknown code" "$(mongo_eval 'db.runCommand({nosuchcommand:1}).code')" "59"
check_eq "unknown codeName" "$(mongo_eval 'db.runCommand({nosuchcommand:1}).codeName')" \
  "CommandNotFound"
# The connection must still be usable after an error.
check_eq "connection survives an error" \
  "$(mongo_eval 'db.runCommand({nosuchcommand:1}); db.runCommand({ping:1}).ok')" "1"

note "7. the server shuts down cleanly with a client still connected"
# A listener thread that cannot be joined would hang shutdown forever, so this
# is the real test of plugin deinit — not a formality.
"$REFERENCE_MONGO" --port "$MONGO_PORT" --quiet --eval 'sleep(30000)' >/dev/null 2>&1 &
idle_client=$!
sleep 1
"$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION" >/dev/null
kill "$idle_client" 2>/dev/null || true
wait "$idle_client" 2>/dev/null || true
if grep -q 'Shutdown complete' "$ERRLOG"; then shutdown_clean=yes; else shutdown_clean=no; fi
check_eq "shutdown completed" "$shutdown_clean" "yes"
"$CHIMERA_DIR/scripts/run-server.sh" --server "$SERVER_VERSION" >/dev/null
chimera_require_running

note "demo-m3 passed on $SERVER_VERSION"
