"""Synchronize collection reads with concurrent SQL namespace replacement."""

import concurrent.futures
import json
import os
import secrets
import select
import subprocess
import time

from pymongo.errors import OperationFailure
from pymongo import timeout
from bson import json_util


def _wait(predicate, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError('timed out waiting for ' + label)


def run(writer, reader, sql, rejected):
    suffix = secrets.token_hex(4)
    # Mixed case also exercises the server's MDL key folding on
    # case-insensitive/case-preserving installations.
    table = 'Auth_Ddl_Guard_' + suffix
    view, original = table + '_view', table + '_original'
    qualify = lambda name: '`auth_test_allowed`.`' + name + '`'
    secret_key = secrets.token_hex(16)
    secret = json.dumps({'_id': 'private-' + suffix})
    public = {'_id': 'public-' + suffix}
    defaults = [os.environ['CHIMERA_AUTH_SQL'], '--no-defaults',
                '--socket=' + os.environ['CHIMERA_AUTH_SOCKET']]
    old_log, old_output = sql('SELECT @@GLOBAL.general_log,@@GLOBAL.log_output').split('\t')
    blocker = None
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=2)
    reading = renaming = None
    try:
        sql('CREATE TABLE ' + qualify(table) + ' (_id VARBINARY(255) PRIMARY KEY,doc JSON NOT NULL)')
        sql('INSERT INTO ' + qualify(table) + " VALUES (X'01','" + json.dumps(public) + "')")
        sql("INSERT INTO auth_test_denied.secret VALUES (X'" + secret_key + "','" + secret + "')")
        sql('CREATE SQL SECURITY INVOKER VIEW ' + qualify(view) +
            ' AS SELECT _id,doc FROM auth_test_denied.secret WHERE _id=X\'' + secret_key + "'")
        # Internal SQL statements are logged when they finish. This existing
        # observability surface lets us prove the first type check has passed
        # while a table lock prevents the later data access from proceeding.
        output = 'FILE,TABLE' if 'FILE' in old_output else 'TABLE'
        sql("SET GLOBAL log_output='" + output + "'; SET GLOBAL general_log=ON")
        blocker = subprocess.Popen([*defaults, '--user=root', '--batch',
                                    '--skip-column-names', '--unbuffered'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        blocker.stdin.write('LOCK TABLES ' + qualify(table) + " WRITE; SELECT 'locked';\n")
        blocker.stdin.flush()
        assert select.select([blocker.stdout], [], [], 8)[0], 'table blocker did not become ready'
        assert blocker.stdout.readline().strip() == 'locked', 'table blocker failed'

        def find():
            with timeout(12):
                return reader.auth_test_allowed.command({'find': table})
        reading = executor.submit(find)
        _wait(lambda: int(sql(
            "SELECT COUNT(*) FROM mysql.general_log WHERE command_type='Query' "
            "AND argument LIKE 'SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = %' "
            "AND LOCATE('" + table + "',argument)>0")) > 0,
            'collection existence check before the blocked data access')
        if reading.done():
            raise AssertionError('read unexpectedly passed the SQL table blocker: ' +
                                 repr(reading.result()))
        rename = ('RENAME TABLE ' + qualify(table) + ' TO ' + qualify(original) +
                  ', ' + qualify(view) + ' TO ' + qualify(table))
        renaming = executor.submit(sql, 'SET SESSION lock_wait_timeout=10; ' + rename)
        _wait(lambda: int(sql(
            "SELECT COUNT(*) FROM information_schema.PROCESSLIST WHERE STATE LIKE '%metadata lock%' "
            "AND INFO LIKE 'RENAME TABLE %" + table + "%'")) > 0,
            'queued namespace replacement')
        # The exclusive DDL request takes priority over the waiting shared
        # reader. Whichever operation wins, a read must never return the view's
        # private underlying document using the engine's identity.
        blocker.stdin.write('UNLOCK TABLES;\n')
        blocker.stdin.flush()
        blocker.stdin.close()
        blocker.wait(timeout=8)
        renaming.result(timeout=12)
        try:
            result = reading.result(timeout=12)
        except OperationFailure as error:
            assert error.code == 13, error
        else:
            assert result['cursor']['firstBatch'] == [public], result
        rejected('replacement view cannot borrow engine privileges',
                 lambda: reader.auth_test_allowed.command({'find': table}))
    finally:
        if blocker and blocker.poll() is None:
            blocker.terminate()
            try:
                blocker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                blocker.kill()
                blocker.wait(timeout=3)
        # Releasing the blocker lets either worker leave its bounded SQL wait.
        executor.shutdown(wait=True, cancel_futures=True)
        sql('SET GLOBAL general_log=' + old_log + "; SET GLOBAL log_output='" + old_output + "'")
        for name in (table, view, original):
            kind = sql("SELECT TABLE_TYPE FROM information_schema.TABLES "
                       "WHERE TABLE_SCHEMA='auth_test_allowed' AND TABLE_NAME='" + name + "'")
            if kind:
                sql(('DROP VIEW ' if kind == 'VIEW' else 'DROP TABLE ') + qualify(name))
        sql("DELETE FROM auth_test_denied.secret WHERE _id=X'" + secret_key + "'")
    print('  ok  collection data access remains safe across queued table/view replacement')

    # Nested update->load guards must not release the write guard or break the
    # transaction. A SQL caller also borrows its own THD through mongo().
    coll = writer.auth_test_allowed.items
    previous = coll.find_one({'_id': 2})['value']
    coll.update_one({'_id': 2}, {'$set': {'value': previous + 1}})
    assert coll.find_one({'_id': 2})['value'] == previous + 1
    coll.update_one({'_id': 2}, {'$set': {'value': previous}})
    sql("CREATE FUNCTION IF NOT EXISTS mongo RETURNS STRING SONAME 'chimera_mongo.so'")
    result = subprocess.run([*defaults, '--user=auth_test_reader',
                             '--password=sql-only-test-password', '-NBe',
                             "SELECT mongo('auth_test_allowed','db.items.findOne({_id:2})')"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert json_util.loads(result.stdout)['value'] == previous, result.stdout
    print('  ok  metadata guards preserve nested writes and the SQL mongo() caller')
