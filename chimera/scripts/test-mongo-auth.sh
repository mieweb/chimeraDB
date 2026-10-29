#!/usr/bin/env bash
# End-to-end auth/TLS acceptance; uses an isolated source-development datadir.
# Requires PyMongo in CHIMERA_AUTH_PYTHON (defaults to python3) and mongosh.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown arguments"
python=${CHIMERA_AUTH_PYTHON:-python3}
mongo_shell=(mongosh)
if [[ -n ${CHIMERA_AUTH_NODE:-} ]]; then mongo_shell=("$CHIMERA_AUTH_NODE" "$(command -v mongosh)"); fi
"$python" -c 'import pymongo' || die 'install chimera/tests/auth/requirements.txt into a venv and set CHIMERA_AUTH_PYTHON'
"$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION" >/dev/null
"$CHIMERA_DIR/scripts/run-server.sh" --server "$SERVER_VERSION" >/dev/null
chimera_sql <<'SQL'
CREATE DATABASE IF NOT EXISTS auth_test_allowed;
CREATE DATABASE IF NOT EXISTS auth_test_denied;
CREATE USER IF NOT EXISTS 'auth_test_writer'@'localhost' IDENTIFIED BY 'sql-only-test-password';
CREATE USER IF NOT EXISTS 'auth_test_reader'@'localhost' IDENTIFIED BY 'sql-only-test-password';
CREATE USER IF NOT EXISTS 'auth_test_role'@'localhost' IDENTIFIED BY 'sql-only-test-password';
CREATE USER IF NOT EXISTS 'auth_test_table'@'localhost' IDENTIFIED BY 'sql-only-test-password';
CREATE USER IF NOT EXISTS 'auth_test_partial'@'localhost' IDENTIFIED BY 'sql-only-test-password';
CREATE ROLE IF NOT EXISTS auth_test_read_role;
GRANT SELECT ON auth_test_allowed.* TO auth_test_read_role;
GRANT auth_test_read_role TO 'auth_test_role'@'localhost';
SET DEFAULT ROLE auth_test_read_role FOR 'auth_test_role'@'localhost';
GRANT SELECT, UPDATE, ALTER, INDEX ON auth_test_allowed.* TO 'auth_test_partial'@'localhost';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, INDEX, ALTER ON auth_test_allowed.* TO 'auth_test_writer'@'localhost';
GRANT SELECT ON auth_test_allowed.* TO 'auth_test_reader'@'localhost';
SQL
for user in auth_test_writer auth_test_reader auth_test_role auth_test_table auth_test_partial; do
  printf '%s\n' 'mongo-test-password' | "$python" "$CHIMERA_DIR/scripts/mongo-auth.py" \
    --user "$user" --password-stdin --client "$MARIADB" -- --no-defaults --socket="$SOCKET" --user=root
 done
export CHIMERA_AUTH_PORT=$MONGO_PORT CHIMERA_AUTH_SOCKET=$SOCKET CHIMERA_AUTH_SQL=$MARIADB
"$python" "$CHIMERA_DIR/tests/auth/integration.py"
if [[ -x $REFERENCE_MONGOD ]]; then
  CHIMERA_AUTH_REFERENCE="$REFERENCE_MONGOD" "$python" "$CHIMERA_DIR/tests/auth/failure_differential.py"
fi
"${mongo_shell[@]}" --quiet "mongodb://auth_test_writer:mongo-test-password@127.0.0.1:$MONGO_PORT/auth_test_allowed?authSource=admin&directConnection=true" \
  --eval 'if (db.runCommand({connectionStatus:1}).authInfo.authenticatedUsers[0].user !== "auth_test_writer") throw new Error("mongosh auth failed"); print("mongosh default SCRAM negotiation passed")'

"$CHIMERA_DIR/scripts/build-health.sh" --test >/dev/null
"${CHIMERA_OUT:-$CHIMERA_DIR/.run}/health/chimeradb-health" 127.0.0.1 "$MONGO_PORT"
note 'TLS with certificate verification'
tls="$INSTANCE_DIR/auth-tls"
mkdir -p "$tls"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tls/key.pem" -out "$tls/cert.pem" \
  -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' -days 1 >/dev/null 2>&1
chmod 600 "$tls/key.pem"
"$CHIMERA_DIR/scripts/stop-server.sh" --server "$SERVER_VERSION" >/dev/null
"$CHIMERA_DIR/scripts/run-server.sh" --server "$SERVER_VERSION" --tls-cert "$tls/cert.pem" --tls-key "$tls/key.pem" >/dev/null
CHIMERA_AUTH_TLS_CERT="$tls/cert.pem" "$python" "$CHIMERA_DIR/tests/auth/integration.py" --tls
"${mongo_shell[@]}" --quiet "mongodb://auth_test_writer:mongo-test-password@127.0.0.1:$MONGO_PORT/auth_test_allowed?authSource=admin&directConnection=true&tls=true" \
  --tlsCAFile "$tls/cert.pem" --eval 'db.runCommand({connectionStatus:1}); print("mongosh TLS certificate verification passed")'
CHIMERA_MONGO_TLS_CA="$tls/cert.pem" "${CHIMERA_OUT:-$CHIMERA_DIR/.run}/health/chimeradb-health" 127.0.0.1 "$MONGO_PORT"
if CHIMERA_MONGO_TLS_CA="$tls/cert.pem" CHIMERA_MONGO_TLS_SERVER_NAME=wrong.example \
  "${CHIMERA_OUT:-$CHIMERA_DIR/.run}/health/chimeradb-health" 127.0.0.1 "$MONGO_PORT"; then
  die 'health probe accepted a certificate hostname mismatch'
fi
"$CHIMERA_DIR/scripts/test-mongo-tls-deadline.sh" --server "$SERVER_VERSION"
"$CHIMERA_DIR/scripts/test-mongo-auth-startup.sh" --server "$SERVER_VERSION"
note "Mongo authentication and TLS passed on $SERVER_VERSION"
