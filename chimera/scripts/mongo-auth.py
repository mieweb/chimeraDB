#!/usr/bin/env python3
"""Provision/rotate/revoke a SCRAM credential using a local SQL administrator.

mongo-auth.py --user alice -- --protocol=socket --user=root --socket=/path/mysql.sock
The Mongo password is prompted (or read with --password-stdin), never put in argv.
The named MariaDB account must already exist. SQL and Mongo passwords are separate.
CHIMERA_DEFAULTS_FILE selects the SQL client config unless client options specify
--no-defaults, --defaults-file, or --defaults-extra-file. Explicit connection
options such as --socket override values in the selected config.
"""
import argparse
import getpass
import hashlib
import hmac
import os
from pathlib import Path
import stringprep
import subprocess
import sys
import unicodedata


def saslprep(password):
    mapped = ''.join(' ' if stringprep.in_table_c12(c) else c
                     for c in password if not stringprep.in_table_b1(c))
    result = unicodedata.ucd_3_2_0.normalize('NFKC', mapped)
    prohibited = (stringprep.in_table_a1, stringprep.in_table_c12,
                  stringprep.in_table_c21, stringprep.in_table_c22,
                  stringprep.in_table_c3, stringprep.in_table_c4,
                  stringprep.in_table_c5, stringprep.in_table_c6,
                  stringprep.in_table_c7, stringprep.in_table_c8,
                  stringprep.in_table_c9)
    if any(test(c) for c in result for test in prohibited):
        raise ValueError('password contains a character prohibited by SASLprep')
    if any(stringprep.in_table_d1(c) for c in result):
        if (any(stringprep.in_table_d2(c) for c in result) or
                not stringprep.in_table_d1(result[0]) or
                not stringprep.in_table_d1(result[-1])):
            raise ValueError('password violates the SASLprep bidirectional rule')
    if not result:
        raise ValueError('empty passwords are not allowed')
    return result.encode('utf-8')


def literal(value):
    if isinstance(value, str):
        value = value.encode('utf-8')
    return "X'" + value.hex() + "'"


def client_options(options):
    """Put MariaDB's initial options before connection options, without a shell."""
    files = {}
    connection = []
    no_defaults = False
    remaining = iter(options)
    for option in remaining:
        name, equals, value = option.partition('=')
        if name in ('--defaults-file', '--defaults-extra-file'):
            if name in files:
                raise ValueError('each SQL defaults-file option may be specified only once')
            if not equals:
                value = next(remaining, '')
            if not value or value.startswith('--'):
                raise ValueError(name + ' requires a file path')
            files[name] = value
        elif option == '--no-defaults':
            no_defaults = True
        else:
            connection.append(option)
    if no_defaults:
        if files:
            raise ValueError('--no-defaults cannot be combined with SQL defaults-file options')
        return ['--no-defaults', *connection]
    # An explicit file selection replaces the formula's default. In particular,
    # --defaults-extra-file must not leave a hidden --defaults-file in effect.
    if not files and os.environ.get('CHIMERA_DEFAULTS_FILE'):
        files['--defaults-file'] = os.environ['CHIMERA_DEFAULTS_FILE']
    return [*(name + '=' + files[name] for name in ('--defaults-file', '--defaults-extra-file')
              if name in files), *connection]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user', required=True, help='existing MariaDB account name; also the Mongo username')
    parser.add_argument('--auth-db', default='admin')
    parser.add_argument('--sql-host', default='localhost', help='exact MariaDB account Host, not the Mongo client address')
    parser.add_argument('--client', default='mariadb')
    parser.add_argument('--iterations', type=int, default=15000)
    parser.add_argument('--password-stdin', action='store_true')
    parser.add_argument('--validate-password', action='store_true', help='validate stdin password without connecting to SQL')
    parser.add_argument('--remove', action='store_true')
    args, sql_args = parser.parse_known_args()
    if sql_args[:1] == ['--']:
        sql_args = sql_args[1:]
    for value, limit in ((args.user, 128), (args.auth_db, 64), (args.sql_host, 255)):
        if not value or len(value.encode()) > limit or '\0' in value:
            parser.error('invalid user, authentication database, or SQL host')
    if not 4096 <= args.iterations <= 1000000:
        parser.error('iterations must be between 4096 and 1000000')
    if args.validate_password:
        saslprep(sys.stdin.read().removesuffix('\n'))
        return
    command = [args.client, *client_options(sql_args), '--batch', '--skip-column-names']

    def sql(statement):
        result = subprocess.run(command, input=statement, text=True, capture_output=True)
        if result.returncode:
            # Client errors can echo the SQL and thus verifier material. Keep
            # the diagnostic generic and never print the submitted statement.
            raise ValueError('SQL operation failed; check administrator access and connection settings')
        return result.stdout.strip()

    # The installed helper keeps the DDL next to it; source installs use ../sql.
    here = Path(__file__).resolve()
    ddl_path = here.parent.parent / 'sql' / 'auth.sql'
    if not ddl_path.exists():
        ddl_path = here.parent.parent / 'share' / 'chimeradb' / 'sql' / 'auth.sql'
    if args.remove:
        sql(ddl_path.read_text() + '\nDELETE FROM chimera_auth.credentials WHERE auth_db=' +
            literal(args.auth_db) + ' AND user_name=' + literal(args.user) + ';')
        print('Mongo credential revoked; active connections lose access on their next command.')
        return
    exists = sql('SELECT COUNT(*) FROM mysql.global_priv WHERE BINARY User=' + literal(args.user) +
                 ' AND BINARY Host=' + literal(args.sql_host) +
                 " AND COALESCE(JSON_VALUE(Priv, '$.account_locked'), 0)=0;")
    if exists != '1':
        raise ValueError('mapped MariaDB account must exist and be unlocked')
    if args.password_stdin:
        password = sys.stdin.read().removesuffix('\n')
    else:
        password = getpass.getpass('Mongo password: ')
        if password != getpass.getpass('Confirm Mongo password: '):
            raise ValueError('passwords do not match')
    # MongoDB's SCRAM-SHA-256 clients include libmongoc, which requires the
    # server's 28-byte salt format rather than accepting arbitrary SCRAM salts.
    salt = os.urandom(28)
    salted = hashlib.pbkdf2_hmac('sha256', saslprep(password), salt, args.iterations)
    client_key = hmac.digest(salted, b'Client Key', 'sha256')
    stored_key = hashlib.sha256(client_key).digest()
    server_key = hmac.digest(salted, b'Server Key', 'sha256')
    values = [literal(args.auth_db), literal(args.user), literal(args.sql_host), literal(salt),
              str(args.iterations), literal(stored_key), literal(server_key), literal(os.urandom(16))]
    sql(ddl_path.read_text() + '\nREPLACE INTO chimera_auth.credentials '
        '(auth_db,user_name,sql_host,salt,iterations,stored_key,server_key,revision) VALUES (' +
        ','.join(values) + ');')
    print('Mongo credential installed. SQL grants control access; reconnect after password changes.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(str(error))
