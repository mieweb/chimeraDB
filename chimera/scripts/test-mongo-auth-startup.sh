#!/usr/bin/env bash
# Refusal paths must leave the Mongo port closed while SQL can still start.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die 'unknown arguments'
python=${CHIMERA_AUTH_PYTHON:-python3}
tls="$INSTANCE_DIR/auth-tls"
[[ -f $tls/cert.pem ]] || die 'run test-mongo-auth.sh first'
start() {
  "$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION" >/dev/null
  "$CHIMERA_DIR/scripts/run-server.sh" --server "$SERVER_VERSION" "$@" >/dev/null
}
refused() {
  local label=$1
  shift
  start "$@"
  [[ $(sql_scalar "SELECT COUNT(*) FROM information_schema.plugins WHERE plugin_name='chimera_mongo' AND plugin_status='ACTIVE'") == 0 ]] ||
    die "$label: plugin unexpectedly ACTIVE"
  CHIMERA_AUTH_PORT=$MONGO_PORT "$python" - <<'PY'
import os,socket
try:
    sock=socket.create_connection(('127.0.0.1',int(os.environ['CHIMERA_AUTH_PORT'])),timeout=.5)
except OSError:
    pass
else:
    sock.close()
    raise AssertionError('refused Mongo listener opened a port')
PY
  printf '  ok  %s leaves Mongo closed\n' "$label"
}
refused 'remote plaintext without override' --mongo-bind 0.0.0.0
refused 'remote unauthenticated without override' --mongo-bind 0.0.0.0 --no-auth
refused 'incomplete TLS configuration' --tls-cert "$tls/cert.pem"
refused 'invalid TLS certificate' --tls-cert "$tls/missing.pem" --tls-key "$tls/key.pem"
# Accept a remote bind only with both authentication and TLS. Authentication is
# exercised through loopback; certificate verification stays on.
start --mongo-bind 0.0.0.0 --tls-cert "$tls/cert.pem" --tls-key "$tls/key.pem"
CHIMERA_AUTH_PORT=$MONGO_PORT CHIMERA_AUTH_TLS_CERT="$tls/cert.pem" "$python" - <<'PY'
import os
from pymongo import MongoClient
from pymongo.errors import OperationFailure
opts=dict(directConnection=True,tls=True,tlsCAFile=os.environ['CHIMERA_AUTH_TLS_CERT'],serverSelectionTimeoutMS=2000)
with MongoClient('127.0.0.1',int(os.environ['CHIMERA_AUTH_PORT']),**opts) as anon:
    try: anon.auth_test_allowed.items.find_one()
    except OperationFailure as e: assert e.code==13
    else: raise AssertionError('remote bind accepted anonymous data access')
with MongoClient('127.0.0.1',int(os.environ['CHIMERA_AUTH_PORT']),username='auth_test_writer',password='mongo-test-password',authSource='admin',**opts) as user:
    assert user.auth_test_allowed.command({'chimeraSql':'SELECT CURRENT_USER() AS who'})['cursor']['firstBatch'][0]['who']=='auth_test_writer@localhost'
print('  ok  remote authenticated TLS listener and restricted SQL gateway')
PY
CHIMERA_MONGO_TLS_CA="$tls/cert.pem" CHIMERA_MONGO_TLS_SERVER_NAME=localhost \
  "${CHIMERA_OUT:-$CHIMERA_DIR/.run}/health/chimeradb-health" 127.0.0.1 "$MONGO_PORT"
# Restore the ordinary local, authenticated development configuration.
start
note "Mongo startup policy passed on $SERVER_VERSION"
