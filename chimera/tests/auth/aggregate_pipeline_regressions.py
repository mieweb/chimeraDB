"""Raw BSON pipeline encodings must not change aggregate authorization."""

import base64
import hashlib
import hmac
import os
import secrets
import select
import socket
import ssl
import struct
import subprocess

from bson import BSON, Binary


class _RawClient:
    def __init__(self, port, tls_ca):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        if tls_ca:
            self.socket = ssl.create_default_context(cafile=tls_ca).wrap_socket(
                self.socket, server_hostname='127.0.0.1')
        self.request_id = 0

    def close(self):
        self.socket.close()

    def _exact(self, size):
        data = b''
        while len(data) < size:
            part = self.socket.recv(size - len(data))
            assert part, 'server closed the raw pipeline regression connection'
            data += part
        return data

    def command(self, command):
        body = command if isinstance(command, bytes) else BSON.encode(command)
        self.request_id += 1
        self.socket.sendall(struct.pack('<iiii', len(body) + 21, self.request_id, 0, 2013) + b'\0' * 5 + body)
        size, _, response_to, opcode = struct.unpack('<iiii', self._exact(16))
        assert opcode == 2013 and response_to == self.request_id
        payload = self._exact(size - 16)
        assert payload[4] == 0, 'expected an OP_MSG body section'
        return BSON(payload[5:]).decode()

    def authenticate(self):
        first = 'n=auth_test_writer,r=' + secrets.token_urlsafe(24)
        challenge = self.command({'saslStart': 1, 'mechanism': 'SCRAM-SHA-256',
                                  'payload': Binary(('n,,' + first).encode()), '$db': 'admin'})
        assert challenge['ok'] == 1, challenge
        server = bytes(challenge['payload']).decode()
        attrs = dict(part.split('=', 1) for part in server.split(','))
        final = 'c=biws,r=' + attrs['r']
        salted = hashlib.pbkdf2_hmac('sha256', b'mongo-test-password',
                                    base64.b64decode(attrs['s']), int(attrs['i']))
        client_key = hmac.digest(salted, b'Client Key', 'sha256')
        message = (first + ',' + server + ',' + final).encode()
        signature = hmac.digest(hashlib.sha256(client_key).digest(), message, 'sha256')
        proof = bytes(a ^ b for a, b in zip(client_key, signature))
        response = self.command({'saslContinue': 1, 'conversationId': challenge['conversationId'],
                                 'payload': Binary((final + ',p=' + base64.b64encode(proof).decode()).encode()),
                                 '$db': 'admin'})
        assert response['ok'] == 1, response
        expected = b'v=' + base64.b64encode(hmac.digest(hmac.digest(salted, b'Server Key', 'sha256'), message, 'sha256'))
        assert bytes(response['payload']) == expected, response
        if not response['done']:
            response = self.command({'saslContinue': 1, 'conversationId': challenge['conversationId'],
                                     'payload': Binary(b''), '$db': 'admin'})
            assert response['ok'] == 1 and response['done'], response


def _document(fields):
    # Unlike a Python mapping, this also preserves duplicate raw BSON keys.
    elements = b''.join(BSON.encode({key: value})[4:-1] for key, value in fields)
    return struct.pack('<i', len(elements) + 5) + elements + b'\0'


def _aggregate(table, pipeline_type, fields):
    elements = (BSON.encode({'aggregate': table})[4:-1] + bytes([pipeline_type]) +
                b'pipeline\0' + _document(fields) +
                BSON.encode({'cursor': {}, '$db': 'auth_test_denied'})[4:-1])
    return struct.pack('<i', len(elements) + 5) + elements + b'\0'


def run(port, sql, tls_ca=None):
    table = 'auth_pipeline_guard_' + secrets.token_hex(4)
    qualify = lambda db: '`' + db + '`.`' + table + '`'
    client = blocker = None
    try:
        for db, identity in [('auth_test_allowed', 'public'), ('auth_test_denied', 'private')]:
            sql('CREATE TABLE ' + qualify(db) + ' (_id VARBINARY(255) PRIMARY KEY,doc JSON NOT NULL)')
            sql('INSERT INTO ' + qualify(db) + " VALUES (X'01','{\"_id\":\"" + identity + "\"}')")
        client = _RawClient(port, tls_ca)
        client.authenticate()
        valid = client.command({'aggregate': table, 'pipeline': [{'$match': {}}, {'$project': {'_id': 1}}],
                                'cursor': {}, '$db': 'auth_test_allowed'})
        assert valid['cursor']['firstBatch'] == [{'_id': 'public'}], valid

        blocker = subprocess.Popen([os.environ['CHIMERA_AUTH_SQL'], '--no-defaults',
                                    '--socket=' + os.environ['CHIMERA_AUTH_SOCKET'], '--user=root',
                                    '--batch', '--skip-column-names', '--unbuffered'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        blocker.stdin.write('LOCK TABLES ' + qualify('auth_test_denied') + " WRITE; SELECT 'locked';\n")
        blocker.stdin.flush()
        assert select.select([blocker.stdout], [], [], 8)[0], 'pipeline table blocker did not become ready'
        assert blocker.stdout.readline().strip() == 'locked', 'pipeline table blocker failed'

        # A response while the protected table is locked proves rejection
        # precedes collection data access. The old key-0 authorization check
        # skips SELECT and blocks loading the collection for these encodings.
        reordered = [('1', {'$match': {}}), ('0', {'$sql': 'SELECT 1'})]
        encodings = [
            ('pipeline document', 3, reordered),
            ('reordered pipeline array', 4, reordered),
            ('pipeline array with a missing index', 4, [('1', {'$match': {}})]),
            ('pipeline array with a duplicate index', 4,
             [('0', {'$match': {}}), ('0', {'$sql': 'SELECT 1'})]),
        ]
        for label, kind, fields in encodings:
            response = client.command(_aggregate(table, kind, fields))
            assert response['ok'] == 0 and response['code'] in (9, 13, 14), (label, response)
            assert 'cursor' not in response, (label, response)
        denied = client.command({'aggregate': table, 'pipeline': [{'$match': {}}],
                                 'cursor': {}, '$db': 'auth_test_denied'})
        assert denied['ok'] == 0 and denied['code'] == 13, denied
        # A genuine SQL source remains usable without collection SELECT: both
        # authorization and execution consume the same parsed source decision.
        valid_sql = client.command({'aggregate': table, 'pipeline': [{'$sql': 'SELECT 1 AS n'}],
                                    'cursor': {}, '$db': 'auth_test_denied'})
        assert valid_sql['cursor']['firstBatch'] == [{'n': 1}], valid_sql
    finally:
        if client:
            client.close()
        if blocker and blocker.poll() is None:
            blocker.terminate()
            try:
                blocker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                blocker.kill()
                blocker.wait(timeout=3)
        for db in ('auth_test_allowed', 'auth_test_denied'):
            sql('DROP TABLE IF EXISTS ' + qualify(db))
    print('  ok  raw aggregate encodings cannot bypass collection authorization')
