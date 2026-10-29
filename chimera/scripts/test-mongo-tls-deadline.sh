#!/usr/bin/env bash
# Read-only regression against the running listener configured with TLS.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die 'unknown arguments'
chimera_require_running
python=${CHIMERA_AUTH_PYTHON:-python3}
cert=${CHIMERA_AUTH_TLS_CERT:-$INSTANCE_DIR/auth-tls/cert.pem}
[[ -f $cert ]] || die 'provide CHIMERA_AUTH_TLS_CERT or run test-mongo-auth.sh first'
CHIMERA_AUTH_PORT=$MONGO_PORT CHIMERA_AUTH_TLS_CERT="$cert" \
  "$python" "$CHIMERA_DIR/tests/auth/tls_handshake_deadline.py"
note "Mongo TLS handshake deadline passed on $SERVER_VERSION"
