"""Real-driver acceptance, including negative authorization and live revocation."""
import os
import base64
import hashlib
import hmac
import secrets
import time
import socket
import struct
import subprocess
import sys
from pathlib import Path

from bson import BSON, Binary, Int64
from pymongo import MongoClient
from pymongo.errors import OperationFailure, ServerSelectionTimeoutError
from cursor_auth_regressions import run as run_cursor_auth_regressions
from table_replacement_regressions import run as run_table_replacement_regressions
from aggregate_pipeline_regressions import run as run_aggregate_pipeline_regressions

port = int(os.environ['CHIMERA_AUTH_PORT'])
options = dict(directConnection=True, serverSelectionTimeoutMS=2000, connectTimeoutMS=2000)
tls = '--tls' in sys.argv
if tls:
    options.update(tls=True, tlsCAFile=os.environ['CHIMERA_AUTH_TLS_CERT'])


def client(user=None, password='mongo-test-password', source='admin', **extra):
    kwargs = dict(options)
    kwargs.update(extra)
    if user:
        kwargs.update(username=user, password=password, authSource=source)
    return MongoClient('127.0.0.1', port, **kwargs)


def sql(statement):
    return subprocess.check_output([os.environ['CHIMERA_AUTH_SQL'], '--no-defaults',
        '--socket=' + os.environ['CHIMERA_AUTH_SOCKET'], '--user=root', '-NBe', statement],
        text=True, timeout=15).strip()


def rejected(label, callback, code=13):
    try:
        callback()
    except OperationFailure as e:
        assert e.code == code, (label, e)
        assert e.details['codeName'] == {13: 'Unauthorized', 18: 'AuthenticationFailed', 43: 'CursorNotFound', 334: 'MechanismUnavailable'}[code], (label, e)
        print('  ok ', label)
    else:
        raise AssertionError(label + ' unexpectedly succeeded')


anonymous = client()
assert anonymous.admin.command('ping')['ok'] == 1
hello = anonymous.admin.command({'hello': 1, 'saslSupportedMechs': 'admin.auth_test_writer'})
assert hello['saslSupportedMechs'] == ['SCRAM-SHA-256']
for command in ({'find': 'items'}, {'insert': 'items', 'documents': [{'_id': 'anonymous'}]},
                {'listDatabases': 1}, {'chimeraSql': 'SELECT * FROM mysql.global_priv'},
                {'getMore': Int64(1), 'collection': 'items'}):
    rejected('anonymous ' + next(iter(command)), lambda c=command: anonymous.auth_test_allowed.command(c))
for label, user, password, source in (
        ('wrong password', 'auth_test_writer', 'wrong', 'admin'),
        ('unknown user', 'unknown_user', 'wrong', 'admin'),
        ('wrong authSource', 'auth_test_writer', 'mongo-test-password', 'other')):
    with client(user, password, source) as bad:
        rejected(label, lambda: bad.auth_test_allowed.command('ping'), 18)
with client('auth_test_writer', authMechanism='SCRAM-SHA-1') as bad:
    rejected('unsupported mechanism', lambda: bad.admin.command('ping'), 334)

writer, reader = client('auth_test_writer'), client('auth_test_reader')
assert writer.admin.command('connectionStatus')['authInfo']['authenticatedUsers'] == [dict(user='auth_test_writer', db='admin')]
coll = writer.auth_test_allowed.items
coll.drop()
coll.insert_many([{'_id': i, 'value': i} for i in range(5)])
assert reader.auth_test_allowed.items.count_documents({}) == 5
assert coll.find_one({'_id': 2})['value'] == 2
coll.update_one({'_id': 2}, {'$set': {'value': 20}})
assert coll.find_one({'_id': 2})['value'] == 20
coll.create_index('value')
coll.drop_index('value_1')
for label, callback in (
    ('read-only insert', lambda: reader.auth_test_allowed.items.insert_one({'_id': 99})),
    ('read-only update', lambda: reader.auth_test_allowed.items.update_one({'_id': 1}, {'$set': {'value': 0}})),
    ('read-only delete', lambda: reader.auth_test_allowed.items.delete_one({'_id': 1})),
    ('read-only drop', lambda: reader.auth_test_allowed.items.drop()),
    ('cross-database read', lambda: writer.auth_test_denied.items.find_one()),
    ('cross-database write', lambda: writer.auth_test_denied.items.insert_one({'_id': 1})),
    ('internal schema collection', lambda: writer.chimera_auth.credentials.find_one()),
    ('internal schema listing', lambda: writer.chimera_auth.list_collection_names()),
    ('SQL private credentials', lambda: writer.auth_test_allowed.command({'chimeraSql': 'SELECT * FROM chimera_auth.credentials'})),
    ('SQL mysql accounts', lambda: writer.auth_test_allowed.command({'chimeraSql': 'SELECT * FROM mysql.global_priv'})),
    ('SQL cross-database', lambda: writer.auth_test_allowed.command({'chimeraSql': 'SELECT * FROM auth_test_denied.items'})),
    ('SQL aggregate bypass', lambda: writer.auth_test_allowed.command({'aggregate': 1, 'pipeline': [{'$sql': 'SELECT * FROM mysql.global_priv'}], 'cursor': {}})),
):
    rejected(label, callback)
row = writer.auth_test_allowed.command({'chimeraSql': 'SELECT CURRENT_USER() AS who'})['cursor']['firstBatch'][0]
assert row['who'] == 'auth_test_writer@localhost', row
assert writer.auth_test_allowed.command({'chimeraSql': 'SELECT COUNT(*) AS n FROM auth_test_allowed.items'})['cursor']['firstBatch'][0]['n'] == 5
print('  ok  SQL gateway native identity and grants')
# Default roles, table-only grants and implicit-creation paths use the same ACLs.
with client('auth_test_role') as role_user:
    assert role_user.auth_test_allowed.items.find_one()
    assert role_user.auth_test_allowed.command({'chimeraSql': 'SELECT CURRENT_ROLE() AS r'})['cursor']['firstBatch'][0]['r'] == 'auth_test_read_role'
    rejected('role read-only write', lambda: role_user.auth_test_allowed.items.insert_one({'_id': 99}))
sql("GRANT SELECT ON auth_test_allowed.items TO 'auth_test_table'@'localhost'")
with client('auth_test_table') as table_user:
    assert table_user.auth_test_allowed.items.find_one()
    assert 'items' in table_user.auth_test_allowed.list_collection_names()
    assert 'auth_test_allowed' in table_user.list_database_names()
    rejected('table grant does not expose another table', lambda: table_user.auth_test_allowed.other.find_one())
with client('auth_test_partial') as limited:
    rejected('update cannot implicitly create a collection', lambda: limited.auth_test_allowed.absent.update_one({'_id': 1}, {'$set': {'x': 1}}))
    rejected('index cannot implicitly create a collection', lambda: limited.auth_test_allowed.absent.create_index('x'))
    rejected('findAndModify cannot implicitly create', lambda: limited.auth_test_allowed.absent.find_one_and_update({'_id': 1}, {'$set': {'x': 1}}))
    rejected('upsert requires INSERT', lambda: limited.auth_test_allowed.items.update_one({'_id': 99}, {'$set': {'x': 1}}, upsert=True))
sql("CREATE TABLE IF NOT EXISTS auth_test_denied.secret (_id VARBINARY(255) PRIMARY KEY, doc JSON)")
sql("CREATE OR REPLACE SQL SECURITY INVOKER VIEW auth_test_allowed.secret_view AS SELECT _id,doc FROM auth_test_denied.secret")
rejected('invoker view cannot borrow engine privileges', lambda: reader.auth_test_allowed.secret_view.find_one())
rejected('SQL invoker view checks underlying grants', lambda: reader.auth_test_allowed.command({'chimeraSql': 'SELECT * FROM auth_test_allowed.secret_view'}))
# Enabling gateway writes still cannot defeat native SQL grants.
sql('SET GLOBAL chimera_mongo_sql_writes=ON')
try:
    rejected('SQL writes enforce native read-only grants', lambda: reader.auth_test_allowed.command({'chimeraSql': 'DELETE FROM auth_test_allowed.items'}))
    assert writer.auth_test_allowed.command({'chimeraSql': 'DELETE FROM auth_test_allowed.items WHERE 1=0'})['ok'] == 1
finally:
    sql('SET GLOBAL chimera_mongo_sql_writes=OFF')
print('  ok  roles, table grants, implicit creation and SQL view isolation')
run_aggregate_pipeline_regressions(port, sql, os.environ['CHIMERA_AUTH_TLS_CERT'] if tls else None)
run_table_replacement_regressions(writer, reader, sql, rejected)


batch = writer.auth_test_allowed.command({'find': 'items', 'batchSize': 1})
cursor = batch['cursor']['id']
assert cursor != 0
rejected('cross-user getMore', lambda: reader.auth_test_allowed.command({'getMore': cursor, 'collection': 'items'}), 43)
killed = reader.auth_test_allowed.command({'killCursors': 'items', 'cursors': [cursor]})
assert killed['cursorsKilled'] == []
assert writer.auth_test_allowed.command({'getMore': cursor, 'collection': 'items'})['cursor']['nextBatch']
print('  ok  cursor ownership')
# SQL-source cursors carry the same owner as ordinary collection cursors.
sql_cursor = writer.auth_test_allowed.command({'aggregate': 1, 'pipeline': [{'$sql': 'SELECT doc FROM auth_test_allowed.items'}], 'cursor': {}, 'batchSize': 1})['cursor']['id']
assert sql_cursor
rejected('cross-user SQL cursor', lambda: reader.auth_test_allowed.command({'getMore': sql_cursor, 'collection': '$cmd.aggregate'}), 43)
assert writer.auth_test_allowed.command({'getMore': sql_cursor, 'collection': '$cmd.aggregate'})['cursor']['nextBatch']
# A collection watch exposes only its authorized namespace.
with reader.auth_test_allowed.items.watch(max_await_time_ms=500) as stream:
    coll.insert_one({'_id': 'authenticated-watch', 'value': 7})
    event = None
    deadline = time.monotonic() + 5
    while event is None and time.monotonic() < deadline:
        event = stream.try_next()
    assert event is not None, 'authenticated change stream did not deliver its event'
    assert event['ns'] == {'db': 'auth_test_allowed', 'coll': 'items'}, event
    assert event['fullDocument']['_id'] == 'authenticated-watch', event
coll.delete_one({'_id': 'authenticated-watch'})
rejected('cross-database watch', lambda: writer.auth_test_denied.items.watch())
rejected('raw oplog requires an explicit grant', lambda: reader.local['oplog.rs'].find_one())
sql("GRANT SELECT ON chimera_meta.oplog TO 'auth_test_reader'@'localhost'")
try:
    assert reader.local['oplog.rs'].find_one({'ns': 'auth_test_allowed.items'})
    rejected('oplog read grant cannot authorize writes', lambda: reader.local['oplog.rs'].insert_one({'_id': 'forbidden'}))
finally:
    sql("REVOKE SELECT ON chimera_meta.oplog FROM 'auth_test_reader'@'localhost'")
print('  ok  authenticated change streams, raw oplog grants and SQL cursor isolation')


run_cursor_auth_regressions(writer, reader, sql, rejected)

# GRANT changes affect an already-authenticated pool, not only new connections.
sql("REVOKE SELECT ON auth_test_allowed.* FROM 'auth_test_reader'@'localhost'")
try:
    rejected('live GRANT revocation', lambda: reader.auth_test_allowed.items.find_one())
finally:
    sql("GRANT SELECT ON auth_test_allowed.* TO 'auth_test_reader'@'localhost'")
# A password update changes the revision, invalidating existing sessions.
sql("UPDATE chimera_auth.credentials SET revision=RANDOM_BYTES(16) WHERE user_name=X'617574685f746573745f726561646572'")
rejected('live credential rotation', lambda: reader.auth_test_allowed.items.find_one())
reader.close()
reader = client('auth_test_reader')
assert reader.auth_test_allowed.items.find_one()
sql("ALTER USER 'auth_test_reader'@'localhost' ACCOUNT LOCK")
try:
    rejected('live SQL account lock', lambda: reader.auth_test_allowed.items.find_one())
    with client('auth_test_reader') as bad:
        rejected('locked SQL account login', lambda: bad.admin.command('ping'), 18)
finally:
    sql("ALTER USER 'auth_test_reader'@'localhost' ACCOUNT UNLOCK")
reader.close()

# A SQL user may invoke mongo(), but its UDF must not turn a read-only SQL
# account into the privileged internal engine identity.
sql("CREATE FUNCTION IF NOT EXISTS mongo RETURNS STRING SONAME 'chimera_mongo.so'")
output = subprocess.run([os.environ['CHIMERA_AUTH_SQL'], '--no-defaults',
    '--socket=' + os.environ['CHIMERA_AUTH_SOCKET'], '--user=auth_test_reader',
    '--password=sql-only-test-password', '-NBe',
    "SELECT mongo('auth_test_denied', 'db.items.findOne({})')"], capture_output=True, text=True)
assert output.returncode != 0 and 'Not authorized' in output.stderr, output
print('  ok  SQL mongo() cannot bypass native grants')


if tls:
    with client('auth_test_writer', tlsCAFile=None) as untrusted:
        try:
            untrusted.admin.command('ping')
        except ServerSelectionTimeoutError:
            print('  ok  untrusted server certificate rejected')
        else:
            raise AssertionError('untrusted server certificate accepted')
else:
    # Raw wire commands let us test malformed SASL without the driver repairing it.
    with socket.create_connection(('127.0.0.1', port)) as sock:
        def raw(command):
            body = BSON.encode(dict(command, **{'$db': 'admin'}))
            frame = struct.pack('<iiii', len(body)+21, 1, 0, 2013) + b'\0'*5 + body
            sock.sendall(frame)
            def exact(n):
                data = b''
                while len(data) < n:
                    chunk = sock.recv(n-len(data))
                    assert chunk
                    data += chunk
                return data
            header = exact(16)
            return BSON(exact(struct.unpack('<i', header[:4])[0]-16)[5:]).decode()
        response = raw({'saslContinue': 1, 'conversationId': 1, 'payload': Binary(b'')})
        assert response['code'] == 18
        response = raw({'saslStart': 1, 'mechanism': 'SCRAM-SHA-256', 'payload': Binary(b'n,,n=auth_test_writer,r=long-enough-nonce,r=duplicate')})
        assert response['code'] == 18
        assert raw({'find': 'items'})['code'] == 13
        print('  ok  malformed and out-of-order SASL fail closed')

        def authenticate(user):
            nonce = secrets.token_urlsafe(24)
            first = 'n=' + user + ',r=' + nonce
            challenge = raw({'saslStart': 1, 'mechanism': 'SCRAM-SHA-256', 'payload': Binary(('n,,' + first).encode())})
            assert challenge['ok'] == 1, challenge
            server = bytes(challenge['payload']).decode()
            attrs = dict(part.split('=', 1) for part in server.split(','))
            final = 'c=biws,r=' + attrs['r']
            salted = hashlib.pbkdf2_hmac('sha256', b'mongo-test-password', base64.b64decode(attrs['s']), int(attrs['i']))
            client_key = hmac.digest(salted, b'Client Key', 'sha256')
            stored = hashlib.sha256(client_key).digest()
            message = (first + ',' + server + ',' + final).encode()
            signature = hmac.digest(stored, message, 'sha256')
            proof = bytes(a ^ b for a,b in zip(client_key, signature))
            response = raw({'saslContinue': 1, 'conversationId': challenge['conversationId'],
                            'payload': Binary((final + ',p=' + base64.b64encode(proof).decode()).encode())})
            assert response['ok'] == 1, response
            expected = b'v=' + base64.b64encode(hmac.digest(hmac.digest(salted,b'Server Key','sha256'), message, 'sha256'))
            assert bytes(response['payload']) == expected
            assert raw({'saslContinue': 1, 'conversationId': challenge['conversationId'], 'payload': Binary(b'')})['done']

        authenticate('auth_test_writer')
        sql('SET GLOBAL chimera_mongo_sql_writes=ON')
        try:
            assert raw({'chimeraSql': 'START TRANSACTION'})['ok'] == 1
            assert raw({'chimeraSql': '''INSERT INTO auth_test_allowed.items (_id,doc) VALUES (X'ff','{"_id":"reauth-private"}')'''})['ok'] == 1
            authenticate('auth_test_reader')
            response = raw({'chimeraSql': "SELECT COUNT(*) AS n FROM auth_test_allowed.items WHERE _id=X'ff'"})
            assert response['cursor']['firstBatch'][0]['n'] == 0, response
            assert raw({'logout': 1})['ok'] == 1
            assert raw({'chimeraSql': 'SELECT 1'})['code'] == 13
            print('  ok  reauthentication closes SQL state and rolls back the previous identity')
        finally:
            sql('SET GLOBAL chimera_mongo_sql_writes=OFF')
writer.close()
anonymous.close()
print('Authentication acceptance passed' + (' with TLS' if tls else ''))
