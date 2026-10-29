"""Database creation keeps the caller's grants across concurrent SQL DROP."""

import concurrent.futures
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import time

from pymongo import timeout
from pymongo.errors import OperationFailure


def _wait(predicate, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('timed out waiting for ' + label)


def run(client, sql):
    suffix = secrets.token_hex(4)
    database = 'auth_schema_race_' + suffix
    user = 'auth_schema_' + suffix
    account = "'" + user + "'@'localhost'"
    schema = '`' + database + '`'
    transaction_table = '`auth_test_allowed`.`auth_schema_txn_' + suffix + '`'
    defaults = [os.environ['CHIMERA_AUTH_SQL'], '--no-defaults',
                '--socket=' + os.environ['CHIMERA_AUTH_SOCKET']]
    old_log, old_output = sql('SELECT @@GLOBAL.general_log,@@GLOBAL.log_output').split('\t')
    old_writes = sql('SELECT @@GLOBAL.chimera_mongo_sql_writes')
    blocker = limited = None
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    pending = None

    def restricted_sql(statement):
        return subprocess.run([*defaults, '--user=' + user,
                               '--password=sql-only-test-password', '-NBe', statement],
                              capture_output=True, text=True, timeout=8)

    def schema_exists():
        return sql("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='" + database + "'") == '1'

    try:
        sql('CREATE DATABASE ' + schema + '; CREATE USER ' + account +
            " IDENTIFIED BY 'sql-only-test-password'; GRANT DROP ON " + schema + '.* TO ' + account)
        for collection in ('existing', 'created'):
            sql('GRANT CREATE,INSERT ON ' + schema + '.`' + collection + '` TO ' + account)
        sql('CREATE TABLE ' + transaction_table + ' (id INT PRIMARY KEY) ENGINE=InnoDB; '
            'GRANT INSERT ON ' + transaction_table + ' TO ' + account)
        helper = Path(__file__).resolve().parents[2] / 'scripts/mongo-auth.py'
        provision = subprocess.run([sys.executable, str(helper), '--user', user,
                                    '--password-stdin', '--client', defaults[0], '--',
                                    *defaults[1:], '--user=root'],
                                   input='mongo-test-password\n', capture_output=True, text=True, timeout=10)
        assert provision.returncode == 0, provision.stderr
        limited = client(user, maxPoolSize=1, socketTimeoutMS=12000)
        limited[database].existing.insert_one({'_id': 'table-grant-control'})
        print('  ok  table-only CREATE permits a new collection in an existing database')

        output = 'FILE,TABLE' if 'FILE' in old_output else 'TABLE'
        sql("SET GLOBAL log_output='" + output + "'; SET GLOBAL general_log=ON")
        blocker = subprocess.Popen([*defaults, '--user=root', '--batch',
                                    '--skip-column-names', '--unbuffered'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        blocker.stdin.write("LOCK TABLES chimera_meta.collections WRITE; SELECT 'locked';\n")
        blocker.stdin.flush()
        assert select.select([blocker.stdout], [], [], 8)[0], 'catalog blocker did not become ready'
        assert blocker.stdout.readline().strip() == 'locked', 'catalog blocker failed'

        # Local SQL-service THDs do not appear in PROCESSLIST. Completed schema
        # checks in the general log establish that authorization saw the schema
        # before DROP; the catalog lock prevents bootstrap from reaching DDL on
        # the user schema while this SQL connection drops it.
        observed = ("SELECT COUNT(*) FROM mysql.general_log WHERE command_type='Query' "
                    "AND argument LIKE 'SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME=%' "
                    "AND LOCATE('" + database + "',argument)>0")
        before = int(sql(observed))

        def insert():
            with timeout(12):
                return limited[database].created.insert_one({'_id': 'concurrent-drop'})

        pending = executor.submit(insert)
        _wait(lambda: int(sql(observed)) > before, 'schema check before privileged collection bootstrap')
        assert not pending.done(), 'insert unexpectedly passed the catalog blocker'
        dropped = restricted_sql('SET SESSION lock_wait_timeout=3; DROP DATABASE ' + schema)
        assert dropped.returncode == 0, dropped.stderr
        assert not schema_exists(), 'DROP DATABASE did not remove the target schema'
        # This account can drop the database and create its named tables, but
        # native SQL must still reject creating the database itself.
        denied = restricted_sql('CREATE DATABASE ' + schema)
        assert denied.returncode != 0 and '1044' in denied.stderr, denied
        assert not schema_exists(), 'restricted SQL unexpectedly recreated the database'
        blocker.stdin.write('UNLOCK TABLES;\n')
        blocker.stdin.flush()
        blocker.stdin.close()
        blocker.wait(timeout=8)
        try:
            pending.result(timeout=12)
        except OperationFailure as error:
            # Depending on when DROP reaches the callback's final schema check,
            # either restricted CREATE is denied or engine CREATE TABLE sees a
            # missing database. A lock timeout must not mask this regression.
            assert error.code in (13, 26), error
        else:
            raise AssertionError('collection creation recreated a database without database CREATE')
        assert not schema_exists(), 'privileged collection bootstrap recreated the dropped database'
        print('  ok  concurrent DROP cannot turn table CREATE into database CREATE')

        try:
            limited[database].created.insert_one({'_id': 'missing-schema-control'})
        except OperationFailure as error:
            assert error.code == 13, error
        else:
            raise AssertionError('table-level CREATE permitted an absent database')
        assert not schema_exists()
        print('  ok  table-only CREATE rejects an absent database')

        sql('GRANT CREATE ON ' + schema + '.* TO ' + account + '; SET GLOBAL chimera_mongo_sql_writes=ON')

        def gateway(statement):
            return limited[database].command({'chimeraSql': statement})

        gateway('START TRANSACTION')
        # Touch InnoDB: START TRANSACTION alone need not activate the engine's
        # transaction state until its first transactional statement.
        gateway('INSERT INTO ' + transaction_table + ' VALUES (1)')
        assert gateway('SELECT @@in_transaction AS active')['cursor']['firstBatch'][0]['active'] == 1
        limited[database].created.insert_one({'_id': 'database-grant-control'})
        assert schema_exists(), 'database CREATE grant did not permit schema creation'
        assert gateway('SELECT @@in_transaction AS active')['cursor']['firstBatch'][0]['active'] == 1
        gateway('ROLLBACK')
        assert sql('SELECT COUNT(*) FROM ' + transaction_table) == '0'
        print('  ok  authorized schema creation preserves the SQL gateway transaction')
    finally:
        if blocker and blocker.poll() is None:
            blocker.terminate()
            try:
                blocker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                blocker.kill()
                blocker.wait(timeout=3)
        executor.shutdown(wait=True, cancel_futures=True)
        if limited:
            limited.close()
        sql('SET GLOBAL chimera_mongo_sql_writes=' + old_writes +
            '; SET GLOBAL general_log=' + old_log + "; SET GLOBAL log_output='" + old_output + "'")
        sql('DROP DATABASE IF EXISTS ' + schema + '; DROP TABLE IF EXISTS ' + transaction_table +
            '; DROP USER IF EXISTS ' + account)
        sql("DELETE FROM chimera_auth.credentials WHERE user_name=X'" + user.encode().hex() + "'")
        sql("DELETE FROM chimera_meta.collections WHERE db_name='" + database + "'; "
            "DELETE FROM chimera_meta.indexes WHERE db_name='" + database + "'")
