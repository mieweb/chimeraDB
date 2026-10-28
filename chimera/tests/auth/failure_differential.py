"""Compare driver-visible login failure envelopes with a black-box mongod."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from pymongo import MongoClient
from pymongo.errors import OperationFailure

binary = os.environ['CHIMERA_AUTH_REFERENCE']
port = int(os.environ['CHIMERA_AUTH_PORT'])
with socket.socket() as reserve:
    reserve.bind(('127.0.0.1', 0))
    reference_port = reserve.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='chimera-auth-oracle-') as directory:
    log = Path(directory) / 'mongod.log'
    def launch(auth):
        return subprocess.Popen([binary, '--dbpath', directory, '--port', str(reference_port),
            '--bind_ip', '127.0.0.1', '--logpath', str(log), '--logappend'] + (['--auth'] if auth else []),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    process = launch(False)
    try:
        reference = MongoClient('127.0.0.1', reference_port, directConnection=True,
                                serverSelectionTimeoutMS=500, connectTimeoutMS=500)
        for attempt in range(30):
            try:
                reference.admin.command('ping')
                break
            except Exception:
                if process.poll() is not None:
                    raise AssertionError('reference mongod exited: ' + log.read_text())
                time.sleep(0.1)
        else:
            raise AssertionError('reference mongod did not become ready')
        reference.admin.command({'createUser': 'auth_test_writer', 'pwd': 'mongo-test-password',
                                 'roles': [{'role': 'readWrite', 'db': 'auth_test_allowed'}]})
        reference.close()
        process.terminate()
        process.wait(timeout=15)
        process = launch(True)
        reference = MongoClient('127.0.0.1', reference_port, directConnection=True, serverSelectionTimeoutMS=500)
        for attempt in range(30):
            try:
                reference.admin.command('ping')
                break
            except Exception:
                if process.poll() is not None: raise AssertionError(log.read_text())
                time.sleep(0.1)
        else:
            raise AssertionError('authenticated reference mongod did not become ready')
        reference.close()
        for label, credential in (
            ('anonymous read', None),
            ('wrong password', ('auth_test_writer', 'incorrect', 'admin')),
            ('unknown user', ('auth_test_nonexistent', 'incorrect', 'admin')),
            ('wrong authSource', ('auth_test_writer', 'mongo-test-password', 'other')),
        ):
            replies = []
            for target in (reference_port, port):
                kwargs = dict(directConnection=True, serverSelectionTimeoutMS=2000)
                if credential:
                    kwargs.update(username=credential[0], password=credential[1], authSource=credential[2])
                with MongoClient('127.0.0.1', target, **kwargs) as client:
                    try:
                        client.auth_test_allowed.command({'find': 'items', 'filter': {}})
                    except OperationFailure as error:
                        replies.append({key: error.details[key] for key in ('ok', 'code', 'codeName')})
                    else:
                        raise AssertionError(label + ' unexpectedly succeeded')
            assert replies[0] == replies[1], (label, replies)
            print('  ok  reference failure envelope:', label, replies[0])
    finally:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
